/*
 * =================================================================
 * SmartVase - Vision Co-Processor (ESP32-CAM)
 * Versione 2.1 — 2026-06-11 (hardening pre-bring-up)
 * =================================================================
 *  Architettura:
 *   - STA Wi-Fi autonomo (credenziali in NVS via Preferences),
 *     riconnessione non bloccante con retry in background.
 *   - CLI seriale per provisioning e test a banco (115200, 'help').
 *   - Cattura JPEG periodica quando rete e upload_url sono configurati.
 *   - Upload HTTP POST multipart streaming a una Cloud Function;
 *     la function restituisce JSON con 'image_url' su storage.
 *   - Pubblicazione MQTT su smartvase/{device_id}/vision/image con
 *     l'URL ottenuto + metadati (timestamp, dimensione, CRC32).
 *   - Stats cumulative persistenti in NVS.
 *
 *  Configurazione (NVS namespace "cam", scrivibile dalla CLI con `set`):
 *     wifi_ssid    string
 *     wifi_pass    string
 *     mqtt_broker  string  (es. "<id>.s1.eu.hivemq.cloud")
 *     mqtt_port    uint16  (default 8883 TLS)
 *     mqtt_user    string
 *     mqtt_pass    string
 *     upload_url   string  (Cloud Function POST endpoint)
 *     interval_s   uint32  (cattura ogni N secondi, default 300)
 *
 *  Stats (namespace "cam_stats"):
 *     succ_frames, fail_frames, upload_err, mqtt_err, total_cap_ms
 * =================================================================
 */

#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "hivemq_ca_cert.h"

#define CAM_FW_VERSION "2.1.0"

// -------------------- DEVICE IDENTITY --------------------
#define DEVICE_ID_PREFIX "CAM_"
static char deviceId[16] = {0};

// -------------------- PINOUT AI-THINKER ESP32-CAM --------------------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// -------------------- TIMING --------------------
#define WIFI_RETRY_INTERVAL_MS   30000UL  // nuovo tentativo STA ogni 30 s
#define MQTT_RETRY_INTERVAL_MS   15000UL  // nuovo tentativo broker ogni 15 s
#define NTP_VALID_EPOCH      1700000000UL // sotto questa soglia l'ora non e' sincronizzata

// -------------------- NVS --------------------
Preferences prefs;
Preferences statsPrefs;

struct CamConfig {
    String wifi_ssid;
    String wifi_pass;
    String mqtt_broker;
    uint16_t mqtt_port;
    String mqtt_user;
    String mqtt_pass;
    String upload_url;
    uint32_t interval_s;
} cfg;

struct CamStats {
    uint32_t successful_frames;
    uint32_t failed_frames;
    uint32_t upload_errors;
    uint32_t mqtt_errors;
    uint64_t total_capture_time_ms;
} stats;

// -------------------- MQTT --------------------
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
String mqttClientId;
String topicVisionImage;
String topicVisionStatus;
String topicVisionCommand;

// Cert HiveMQ Cloud (ISRG Root X1). Shared con l'Hub via infra/hivemq_ca_cert.h.
static const char* hivemq_ca_cert = SMARTVASE_HIVEMQ_CA_CERT;

// -------------------- STATE --------------------
unsigned long lastCaptureMs     = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
bool wifiAttemptInProgress      = false;
bool ntpConfigured              = false;
bool cameraOk                   = false;

// -------------------- UTILITIES --------------------
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4d626158, 0x50657134,
        0xedb88320, 0xf0bfa344, 0xd6d6d3e8, 0xcba1c38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa0d1e278, 0xbdbcb21c
    };
    crc = ~crc;
    while (len--) {
        uint8_t b = *buf++;
        crc = table[(crc ^ b) & 0x0f] ^ (crc >> 4);
        crc = table[(crc ^ (b >> 4)) & 0x0f] ^ (crc >> 4);
    }
    return ~crc;
}

void loadConfig() {
    prefs.begin("cam", true);
    cfg.wifi_ssid   = prefs.getString("wifi_ssid",   "");
    cfg.wifi_pass   = prefs.getString("wifi_pass",   "");
    cfg.mqtt_broker = prefs.getString("mqtt_broker", "");
    cfg.mqtt_port   = prefs.getUShort("mqtt_port",   8883);
    cfg.mqtt_user   = prefs.getString("mqtt_user",   "");
    cfg.mqtt_pass   = prefs.getString("mqtt_pass",   "");
    cfg.upload_url  = prefs.getString("upload_url",  "");
    cfg.interval_s  = prefs.getUInt  ("interval_s",  300);
    prefs.end();
}

void saveConfig() {
    prefs.begin("cam", false);
    prefs.putString("wifi_ssid",   cfg.wifi_ssid);
    prefs.putString("wifi_pass",   cfg.wifi_pass);
    prefs.putString("mqtt_broker", cfg.mqtt_broker);
    prefs.putUShort("mqtt_port",   cfg.mqtt_port);
    prefs.putString("mqtt_user",   cfg.mqtt_user);
    prefs.putString("mqtt_pass",   cfg.mqtt_pass);
    prefs.putString("upload_url",  cfg.upload_url);
    prefs.putUInt  ("interval_s",  cfg.interval_s);
    prefs.end();
}

void loadStats() {
    statsPrefs.begin("cam_stats", true);
    stats.successful_frames     = statsPrefs.getUInt   ("succ_frames",   0);
    stats.failed_frames         = statsPrefs.getUInt   ("fail_frames",   0);
    stats.upload_errors         = statsPrefs.getUInt   ("upload_err",    0);
    stats.mqtt_errors           = statsPrefs.getUInt   ("mqtt_err",      0);
    stats.total_capture_time_ms = statsPrefs.getULong64("total_cap_ms",  0);
    statsPrefs.end();
}

void saveStats() {
    statsPrefs.begin("cam_stats", false);
    statsPrefs.putUInt   ("succ_frames",  stats.successful_frames);
    statsPrefs.putUInt   ("fail_frames",  stats.failed_frames);
    statsPrefs.putUInt   ("upload_err",   stats.upload_errors);
    statsPrefs.putUInt   ("mqtt_err",     stats.mqtt_errors);
    statsPrefs.putULong64("total_cap_ms", stats.total_capture_time_ms);
    statsPrefs.end();
}

void makeDeviceId() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(deviceId, sizeof(deviceId), "%s%02X%02X%02X",
             DEVICE_ID_PREFIX, mac[3], mac[4], mac[5]);
}

// -------------------- CAMERA --------------------
bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0  = Y2_GPIO_NUM;  c.pin_d1  = Y3_GPIO_NUM;
    c.pin_d2  = Y4_GPIO_NUM;  c.pin_d3  = Y5_GPIO_NUM;
    c.pin_d4  = Y6_GPIO_NUM;  c.pin_d5  = Y7_GPIO_NUM;
    c.pin_d6  = Y8_GPIO_NUM;  c.pin_d7  = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href  = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn  = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    if (psramFound()) {
        c.frame_size   = FRAMESIZE_SVGA;  // 800x600
        c.jpeg_quality = 12;              // 0..63 (piu' alto = piu' compresso)
        c.fb_count     = 2;
    } else {
        c.frame_size   = FRAMESIZE_VGA;   // 640x480
        c.jpeg_quality = 14;
        c.fb_count     = 1;
    }
    esp_err_t err = esp_camera_init(&c);
    return err == ESP_OK;
}

// -------------------- WIFI (non bloccante) --------------------
// Avvia un tentativo di connessione senza attendere l'esito: lo stato viene
// verificato a ogni giro di loop da wifiEnsure(). La CLI resta sempre reattiva.
void wifiStartAttempt() {
    if (cfg.wifi_ssid.length() == 0) return;
    Serial.printf("[CAM] Wi-Fi: tentativo di connessione a '%s'...\n", cfg.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    lastWifiAttemptMs    = millis();
    wifiAttemptInProgress = true;
}

void wifiEnsure() {
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiAttemptInProgress) {
            wifiAttemptInProgress = false;
            Serial.printf("[CAM] Wi-Fi OK. IP=%s\n", WiFi.localIP().toString().c_str());
        }
        if (!ntpConfigured) {
            // NTP per il timestamp_utc nel payload vision/image (best effort).
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            ntpConfigured = true;
        }
        return;
    }
    if (cfg.wifi_ssid.length() == 0) return; // non configurato: resta offline
    if (millis() - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS || lastWifiAttemptMs == 0) {
        wifiStartAttempt();
    }
}

// -------------------- MQTT --------------------
void buildTopics() {
    topicVisionImage   = String("smartvase/") + deviceId + "/vision/image";
    topicVisionStatus  = String("smartvase/") + deviceId + "/vision/status";
    topicVisionCommand = String("smartvase/") + deviceId + "/vision/command/#";
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
    // Comandi remoti: captureNow, reboot.
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, len)) return;
    const char* type = doc["type"] | "";
    if (strcmp(type, "captureNow") == 0) {
        lastCaptureMs = 0; // forza cattura al prossimo loop
    } else if (strcmp(type, "reboot") == 0) {
        ESP.restart();
    }
}

void mqttInit() {
    wifiClient.setCACert(hivemq_ca_cert);
    mqttClient.setServer(cfg.mqtt_broker.c_str(), cfg.mqtt_port);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(onMqttMessage);
    mqttClientId = String("SmartVase_") + deviceId;
}

bool mqttEnsure() {
    if (cfg.mqtt_broker.length() == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (mqttClient.connected()) return true;
    // La connect di PubSubClient e' bloccante (anche secondi se il broker non
    // risponde): throttling per non congelare la CLI a ogni giro di loop.
    if (millis() - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS && lastMqttAttemptMs != 0) {
        return false;
    }
    lastMqttAttemptMs = millis();
    Serial.printf("[CAM] MQTT connect %s:%d as %s...\n",
                  cfg.mqtt_broker.c_str(), cfg.mqtt_port, mqttClientId.c_str());
    bool ok = mqttClient.connect(mqttClientId.c_str(),
                                  cfg.mqtt_user.c_str(),
                                  cfg.mqtt_pass.c_str(),
                                  topicVisionStatus.c_str(), 1, true, "offline");
    if (ok) {
        mqttClient.publish(topicVisionStatus.c_str(), "online", true);
        mqttClient.subscribe(topicVisionCommand.c_str());
        Serial.println("[CAM] MQTT connected.");
    } else {
        Serial.printf("[CAM] MQTT connect FAILED, state=%d\n", mqttClient.state());
        stats.mqtt_errors++;
    }
    return ok;
}

// -------------------- UPLOAD --------------------
// POST multipart/form-data del JPEG verso cfg.upload_url, **streaming**
// (no allocazione del body intero in heap). La Cloud Function risponde con
// JSON: { "image_url": "...", "ok": true }
// Ritorna stringa vuota in caso di errore.
//
// Limiti correnti:
//   - setInsecure(): per ora non si valida il cert TLS dell'endpoint;
//     TODO Fia: pin del cert/CA della Cloud Function in cfg/NVS.
String uploadJpeg(const uint8_t* buf, size_t len, const char* contentType) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure(); // TODO: pin certificato dell'endpoint Cloud Function
    client.setTimeout(15000);
    http.setConnectTimeout(8000);
    http.setTimeout(20000);

    if (!http.begin(client, cfg.upload_url)) {
        stats.upload_errors++;
        return String();
    }

    const char* boundary = "----SmartVaseBoundary";
    String headStr =
        String("--") + boundary + "\r\n" +
        "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n" +
        deviceId + "\r\n" +
        "--" + boundary + "\r\n" +
        "Content-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\n" +
        "Content-Type: " + contentType + "\r\n\r\n";
    String tailStr = String("\r\n--") + boundary + "--\r\n";

    size_t totalLen = headStr.length() + len + tailStr.length();
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
    http.addHeader("Content-Length", String(totalLen));

    // sendRequest("POST", Stream*, size) accetta uno Stream che HTTPClient
    // legge a chunk. Per evitare la malloc del body, una piccola classe
    // Stream concatena head + jpeg buffer + tail al volo.
    struct MultipartStream : public Stream {
        const String& head;
        const uint8_t* body;
        size_t bodyLen;
        const String& tail;
        size_t pos;
        size_t total;
        MultipartStream(const String& h, const uint8_t* b, size_t bl, const String& t)
          : head(h), body(b), bodyLen(bl), tail(t), pos(0),
            total(h.length() + bl + t.length()) {}
        int available() override { return (int)(total - pos); }
        int peek() override { return -1; }
        int read() override {
            if (pos >= total) return -1;
            uint8_t b;
            readBytes((char*)&b, 1);
            return b;
        }
        size_t readBytes(char* dest, size_t want) {
            size_t out = 0;
            while (out < want && pos < total) {
                size_t off = pos;
                if (off < (size_t)head.length()) {
                    size_t n = min(want - out, (size_t)head.length() - off);
                    memcpy(dest + out, head.c_str() + off, n);
                    out += n; pos += n;
                } else if (off < (size_t)head.length() + bodyLen) {
                    size_t bOff = off - head.length();
                    size_t n = min(want - out, bodyLen - bOff);
                    memcpy(dest + out, body + bOff, n);
                    out += n; pos += n;
                } else {
                    size_t tOff = off - head.length() - bodyLen;
                    size_t n = min(want - out, (size_t)tail.length() - tOff);
                    memcpy(dest + out, tail.c_str() + tOff, n);
                    out += n; pos += n;
                }
            }
            return out;
        }
        size_t write(uint8_t) override { return 0; }
    } payload(headStr, buf, len, tailStr);

    int httpCode = http.sendRequest("POST", (Stream*)&payload, totalLen);
    String response = (httpCode > 0) ? http.getString() : String();
    http.end();

    if (httpCode != 200) {
        Serial.printf("[CAM] upload HTTP=%d\n", httpCode);
        stats.upload_errors++;
        return String();
    }
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, response)) {
        stats.upload_errors++;
        return String();
    }
    return String((const char*)(doc["image_url"] | ""));
}

void publishVisionImage(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs) {
    if (!mqttEnsure()) return;
    time_t nowEpoch = time(nullptr);
    StaticJsonDocument<512> doc;
    doc["timestamp_utc"]   = (nowEpoch >= (time_t)NTP_VALID_EPOCH) ? (uint32_t)nowEpoch : 0;
    doc["device_id"]       = deviceId;
    doc["fw_version"]      = CAM_FW_VERSION;
    doc["image_url"]       = imageUrl;
    doc["size_bytes"]      = bytes;
    doc["crc32"]           = crc;
    doc["capture_time_ms"] = capMs;
    doc["content_type"]    = "image/jpeg";
    char payload[600];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0 || n >= sizeof(payload)) return;
    if (!mqttClient.publish(topicVisionImage.c_str(), payload)) {
        stats.mqtt_errors++;
    }
}

// -------------------- CAPTURE --------------------
// Cattura un frame e (se richiesto) lo uploada e pubblica su MQTT.
// I metadati del frame vengono copiati PRIMA di restituire il buffer
// al driver: fb non e' piu' valido dopo esp_camera_fb_return.
bool doCapture(bool uploadAndPublish) {
    if (!cameraOk) {
        Serial.println("[CAM] camera non inizializzata.");
        return false;
    }
    unsigned long t0 = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    unsigned long capMs = millis() - t0;
    if (!fb) {
        stats.failed_frames++;
        saveStats();
        Serial.println("[CAM] cattura FALLITA (fb nullo).");
        return false;
    }
    size_t   frameLen = fb->len;
    uint32_t crc      = crc32_le(0, fb->buf, fb->len);

    stats.successful_frames++;
    stats.total_capture_time_ms += capMs;

    Serial.printf("[CAM] frame ok: %u byte, crc32=0x%08lX, %lu ms\n",
                  (unsigned)frameLen, (unsigned long)crc, capMs);

    String url;
    if (uploadAndPublish && cfg.upload_url.length() > 0 &&
        WiFi.status() == WL_CONNECTED) {
        url = uploadJpeg(fb->buf, frameLen, "image/jpeg");
    }
    esp_camera_fb_return(fb);

    if (url.length() > 0) {
        publishVisionImage(url, frameLen, crc, (uint32_t)capMs);
        Serial.printf("[CAM] upload ok: %s\n", url.c_str());
    } else if (uploadAndPublish && cfg.upload_url.length() > 0) {
        Serial.println("[CAM] upload non riuscito (vedi stats).");
    }
    saveStats();
    return true;
}

// -------------------- CLI SERIALE --------------------
static char cliBuf[192];
static size_t cliPos = 0;

void cliPrintHelp() {
    Serial.println("--- SmartVase CAM CLI v" CAM_FW_VERSION " ---");
    Serial.println("help                  questo menu");
    Serial.println("version               versione firmware");
    Serial.println("status                Wi-Fi, MQTT, camera, heap");
    Serial.println("show                  configurazione NVS");
    Serial.println("set <chiave> <val>    wifi_ssid|wifi_pass|mqtt_broker|mqtt_port|");
    Serial.println("                      mqtt_user|mqtt_pass|upload_url|interval_s");
    Serial.println("save                  salva config su NVS");
    Serial.println("wifi connect          ritenta subito la connessione");
    Serial.println("capture               cattura di test (senza upload)");
    Serial.println("upload                cattura + upload + publish completo");
    Serial.println("stats                 statistiche cumulative");
    Serial.println("reboot                riavvia la CAM");
}

void cliPrintStatus() {
    Serial.println("--- status ---");
    Serial.printf("fw_version = %s\n", CAM_FW_VERSION);
    Serial.printf("device_id  = %s\n", deviceId);
    Serial.printf("camera     = %s\n", cameraOk ? "OK" : "FAILED");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi       = CONNESSO ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.printf("wifi       = OFFLINE%s\n",
                      cfg.wifi_ssid.length() == 0 ? " (non configurato)" : "");
    }
    if (cfg.mqtt_broker.length() == 0) Serial.println("mqtt       = NON CONFIGURATO");
    else Serial.printf("mqtt       = %s\n", mqttClient.connected() ? "CONNESSO" : "DISCONNESSO");
    time_t nowEpoch = time(nullptr);
    Serial.printf("ntp_epoch  = %lu%s\n", (unsigned long)nowEpoch,
                  nowEpoch < (time_t)NTP_VALID_EPOCH ? " (non sincronizzato)" : "");
    Serial.printf("free_heap  = %u B\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("uptime_s   = %lu\n", millis() / 1000UL);
}

void cliPrintShow() {
    Serial.println("--- config NVS ---");
    Serial.printf("wifi_ssid   = %s\n", cfg.wifi_ssid.c_str());
    Serial.printf("wifi_pass   = %s\n", cfg.wifi_pass.length() ? "***" : "(vuoto)");
    Serial.printf("mqtt_broker = %s\n", cfg.mqtt_broker.c_str());
    Serial.printf("mqtt_port   = %u\n", cfg.mqtt_port);
    Serial.printf("mqtt_user   = %s\n", cfg.mqtt_user.c_str());
    Serial.printf("mqtt_pass   = %s\n", cfg.mqtt_pass.length() ? "***" : "(vuoto)");
    Serial.printf("upload_url  = %s\n", cfg.upload_url.c_str());
    Serial.printf("interval_s  = %lu\n", (unsigned long)cfg.interval_s);
}

void cliPrintStats() {
    Serial.println("--- stats ---");
    Serial.printf("successful_frames     = %lu\n", (unsigned long)stats.successful_frames);
    Serial.printf("failed_frames         = %lu\n", (unsigned long)stats.failed_frames);
    Serial.printf("upload_errors         = %lu\n", (unsigned long)stats.upload_errors);
    Serial.printf("mqtt_errors           = %lu\n", (unsigned long)stats.mqtt_errors);
    Serial.printf("total_capture_time_ms = %llu\n", stats.total_capture_time_ms);
}

bool cliHandleSet(char* args) {
    char* space = strchr(args, ' ');
    if (space == nullptr) return false;
    *space = '\0';
    const char* key   = args;
    const char* value = space + 1;
    if (strlen(value) == 0) return false;

    if      (strcmp(key, "wifi_ssid")   == 0) cfg.wifi_ssid   = value;
    else if (strcmp(key, "wifi_pass")   == 0) cfg.wifi_pass   = value;
    else if (strcmp(key, "mqtt_broker") == 0) cfg.mqtt_broker = value;
    else if (strcmp(key, "mqtt_port")   == 0) {
        int p = atoi(value);
        if (p <= 0 || p > 65535) { Serial.println("[CLI] porta non valida"); return true; }
        cfg.mqtt_port = (uint16_t)p;
    }
    else if (strcmp(key, "mqtt_user")   == 0) cfg.mqtt_user   = value;
    else if (strcmp(key, "mqtt_pass")   == 0) cfg.mqtt_pass   = value;
    else if (strcmp(key, "upload_url")  == 0) cfg.upload_url  = value;
    else if (strcmp(key, "interval_s")  == 0) {
        long s = atol(value);
        if (s < 10) { Serial.println("[CLI] minimo 10 s"); return true; }
        cfg.interval_s = (uint32_t)s;
    }
    else return false;

    Serial.printf("[CLI] %s impostato (ricorda 'save' + 'reboot')\n", key);
    return true;
}

void cliExecute(char* line) {
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { cliPrintHelp();   return; }
    if (strcmp(line, "version") == 0) {
        Serial.println("SmartVase CAM v" CAM_FW_VERSION " (" __DATE__ " " __TIME__ ")");
        return;
    }
    if (strcmp(line, "status")  == 0) { cliPrintStatus(); return; }
    if (strcmp(line, "show")    == 0) { cliPrintShow();   return; }
    if (strcmp(line, "stats")   == 0) { cliPrintStats();  return; }
    if (strcmp(line, "capture") == 0) { doCapture(false); return; }
    if (strcmp(line, "upload")  == 0) {
        if (cfg.upload_url.length() == 0)      Serial.println("[CLI] upload_url non configurato");
        else if (WiFi.status() != WL_CONNECTED) Serial.println("[CLI] Wi-Fi offline");
        else doCapture(true);
        return;
    }
    if (strcmp(line, "save") == 0) {
        saveConfig();
        Serial.println("[CLI] config salvata su NVS (riavvia con 'reboot')");
        return;
    }
    if (strcmp(line, "wifi connect") == 0) {
        if (cfg.wifi_ssid.length() == 0) Serial.println("[CLI] wifi_ssid non configurato");
        else wifiStartAttempt();
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        Serial.println("[CLI] riavvio...");
        delay(200);
        ESP.restart();
        return;
    }
    Serial.printf("[CLI] comando sconosciuto: '%s' (prova 'help')\n", line);
}

void cliTick() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            cliBuf[cliPos] = '\0';
            if (cliPos > 0) cliExecute(cliBuf);
            cliPos = 0;
            cliBuf[0] = '\0';
            Serial.print("> ");
        } else if (cliPos < sizeof(cliBuf) - 1) {
            cliBuf[cliPos++] = c;
        } else {
            cliPos = 0;
            cliBuf[0] = '\0';
            Serial.println("[CLI] riga troppo lunga, scartata");
        }
    }
}

// -------------------- SETUP / LOOP --------------------
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disabilita brown-out (noto issue ESP32-CAM)
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[CAM] SmartVase Vision Co-Processor v" CAM_FW_VERSION);

    loadConfig();
    loadStats();
    makeDeviceId();
    buildTopics();
    Serial.printf("[CAM] device_id=%s\n", deviceId);

    cameraOk = initCamera();
    if (!cameraOk) {
        // La CLI resta disponibile per diagnosi anche con camera guasta.
        Serial.println("[CAM] Camera init FAILED. CLI attiva per debug.");
    }

    if (cfg.wifi_ssid.length() > 0) {
        wifiStartAttempt();
    } else {
        Serial.println("[CAM] Wi-Fi non configurato. Dalla CLI: set wifi_ssid <...>, set wifi_pass <...>, save, reboot");
    }
    if (cfg.mqtt_broker.length() > 0) {
        mqttInit();
    }

    Serial.println("[CAM] CLI pronta: digita 'help'");
    Serial.print("> ");
}

void loop() {
    cliTick();

    // Rete in background, mai bloccante per la CLI.
    wifiEnsure();
    if (cfg.mqtt_broker.length() > 0 && WiFi.status() == WL_CONNECTED) {
        mqttEnsure();
        if (mqttClient.connected()) mqttClient.loop();
    }

    // Cattura periodica automatica: solo a catena completa configurata
    // (Wi-Fi connesso + upload_url presente). I test manuali a banco si
    // fanno dalla CLI con 'capture' / 'upload'.
    if (cfg.upload_url.length() > 0 && WiFi.status() == WL_CONNECTED &&
        millis() - lastCaptureMs >= (cfg.interval_s * 1000UL)) {
        doCapture(true);
        lastCaptureMs = millis();
    }

    delay(20);
}
