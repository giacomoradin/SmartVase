/*
 * =================================================================
 * SmartVase - Vision Co-Processor (ESP32-CAM)
 * Versione 2.0 — 2026-05-19 (refactor totale per Wi-Fi/MQTT/Cloud)
 * =================================================================
 *  Architettura target:
 *   - STA Wi-Fi autonomo (credenziali in NVS via Preferences).
 *   - Cattura JPEG periodica.
 *   - Upload HTTP POST multipart a una Cloud Function configurabile;
 *     la function restituisce JSON con 'image_url' su storage.
 *   - Pubblicazione MQTT su smartvase/{device_id}/vision/image con
 *     l'URL ottenuto + metadati (timestamp, dimensione, CRC32).
 *   - Stats cumulative persistenti in NVS.
 *
 *  Configurazione (NVS namespace "cam"):
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
 *     succ_frames, fail_frames, upload_errors,
 *     mqtt_errors, total_cap_ms
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
unsigned long lastCaptureMs = 0;

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

// -------------------- WIFI --------------------
bool connectWifi() {
    if (cfg.wifi_ssid.length() == 0) {
        Serial.println("[CAM] No Wi-Fi credentials configured.");
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 30000) {
            Serial.println("[CAM] Wi-Fi connect timeout.");
            return false;
        }
        delay(250);
        Serial.print('.');
    }
    Serial.printf("\n[CAM] Wi-Fi OK. IP=%s\n", WiFi.localIP().toString().c_str());
    // NTP: necessario per ottenere timestamp UTC nel payload vision/image.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    // Attesa breve e non-bloccante della sync (max 3 s).
    unsigned long t0 = millis();
    while (time(nullptr) < 1700000000UL && millis() - t0 < 3000) {
        delay(100);
    }
    return true;
}

// -------------------- MQTT --------------------
void buildTopics() {
    topicVisionImage   = String("smartvase/") + deviceId + "/vision/image";
    topicVisionStatus  = String("smartvase/") + deviceId + "/vision/status";
    topicVisionCommand = String("smartvase/") + deviceId + "/vision/command/#";
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
    // Comandi futuri: capture-now, reboot, set-interval, ecc.
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, len)) return;
    const char* type = doc["type"] | "";
    if (strcmp(type, "captureNow") == 0) {
        lastCaptureMs = 0; // forza cattura al prossimo loop
    } else if (strcmp(type, "reboot") == 0) {
        ESP.restart();
    }
}

bool mqttReconnect() {
    if (mqttClient.connected()) return true;
    if (cfg.mqtt_broker.length() == 0) return false;
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

void mqttInit() {
    wifiClient.setCACert(hivemq_ca_cert);
    mqttClient.setServer(cfg.mqtt_broker.c_str(), cfg.mqtt_port);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(onMqttMessage);
    mqttClientId = String("SmartVase_") + deviceId;
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
    if (cfg.upload_url.length() == 0) {
        Serial.println("[CAM] upload_url not configured.");
        stats.upload_errors++;
        return String();
    }

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

    // sendRequest("POST", Stream*, size) accetta uno Stream che HTTPClient legge
    // a chunk. Per evitare la malloc del body, usiamo una piccola classe Stream
    // che concatena head + jpeg buffer + tail al volo.
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
    if (!mqttReconnect()) return;
    StaticJsonDocument<512> doc;
    doc["timestamp_utc"]   = (uint32_t)(time(nullptr));  // se NTP attivo
    doc["device_id"]       = deviceId;
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

// -------------------- CAPTURE LOOP --------------------
void doCaptureAndPublish() {
    unsigned long t0 = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    unsigned long capMs = millis() - t0;
    if (!fb) {
        stats.failed_frames++;
        saveStats();
        return;
    }
    stats.successful_frames++;
    stats.total_capture_time_ms += capMs;

    uint32_t crc = crc32_le(0, fb->buf, fb->len);
    String url = uploadJpeg(fb->buf, fb->len, "image/jpeg");
    esp_camera_fb_return(fb);

    if (url.length() > 0) {
        publishVisionImage(url, fb->len, crc, (uint32_t)capMs);
    }
    saveStats();
}

// -------------------- SETUP / LOOP --------------------
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disabilita brown-out (noto issue ESP32-CAM)
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[CAM] SmartVase Vision Co-Processor v2.0");

    loadConfig();
    loadStats();
    makeDeviceId();
    buildTopics();
    Serial.printf("[CAM] device_id=%s\n", deviceId);

    if (!initCamera()) {
        Serial.println("[CAM] Camera init FAILED. Looping.");
        while (true) { delay(1000); }
    }

    if (!connectWifi()) {
        Serial.println("[CAM] Operating without network. Will retry in loop().");
    } else {
        mqttInit();
        mqttReconnect();
    }
}

void loop() {
    // Mantieni Wi-Fi + MQTT
    if (WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }
    if (cfg.mqtt_broker.length() > 0) {
        if (!mqttClient.connected()) mqttReconnect();
        mqttClient.loop();
    }

    // Cattura periodica
    if (millis() - lastCaptureMs >= (cfg.interval_s * 1000UL)) {
        if (WiFi.status() == WL_CONNECTED) {
            doCaptureAndPublish();
        } else {
            stats.failed_frames++;
        }
        lastCaptureMs = millis();
    }

    delay(50);
}
