/**
 * @file main.cpp
 * @brief Sorgente per il modulo main
 * @author Giacomo Radin
 * @date 2026-06-30
 */

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
 *   - Pubblicazione Firestore su smartvase/{device_id}/vision/image con
 *     l'URL ottenuto + metadati (timestamp, dimensione, CRC32).
 *   - Stats cumulative persistenti in NVS.
 *   - Telemetria di debug periodica sul monitor seriale (ogni 5 s).
 *
 *  Configurazione (NVS namespace "cam", scrivibile dalla CLI con `set`):
 *     wifi_ssid    string
 *     wifi_pass    string
 *     firebase_api_key  string
 *     firebase_project_id  string
 *     firebase_email  string
 *     firebase_password  string
 *     upload_url   string  (Cloud Function POST endpoint)
 *     interval_s   uint32  (cattura ogni N secondi, default 300)
 *
 *  Stats (namespace "cam_stats"):
 *     succ_frames, fail_frames, upload_err, total_cap_ms
 * =================================================================
 */

#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

#include <FirebaseClient.h>


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
#define NTP_VALID_EPOCH      1700000000UL // sotto questa soglia l'ora non e' sincronizzata

// -------------------- NVS CONFIG --------------------
struct CamConfig {
    String wifi_ssid;
    String wifi_pass;
    String firebase_api_key;
    String firebase_project_id;
    String firebase_email;
    String firebase_password;
    uint32_t interval_s;
} cfg;

// -------------------- FIREBASE GLOBALS --------------------
WiFiClientSecure ssl_client;
DefaultNetwork network;
AsyncClient aClient(ssl_client, getNetwork(network));

FirebaseApp app;
UserAuth user_auth;

// -------------------- NVS PREFERENCES --------------------
Preferences prefs;
Preferences statsPrefs;

struct CamStats {
    uint32_t successful_frames;
    uint32_t failed_frames;
    uint32_t upload_errors;
    uint64_t total_capture_time_ms;
} stats;

// -------------------- STATE --------------------
unsigned long lastCaptureMs     = 0;
unsigned long lastWifiAttemptMs = 0;
bool cameraOk                   = false;
bool captureRequested           = false; // captureNow via Firestore
unsigned long lastDebugMs       = 0;     // throttle telemetria debug seriale

// -------------------- UTILITIES --------------------
/**
 * @brief Calcola il checksum CRC32 (Little Endian) di un buffer di dati.
 * @param crc Valore iniziale del CRC.
 * @param buf Puntatore al buffer dei dati.
 * @param len Lunghezza in byte dei dati nel buffer.
 * @return uint32_t Il checksum CRC32 calcolato.
 */
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

/**
 * @brief Carica le configurazioni dell'ESP32-CAM dalla memoria NVS.
 */
void loadConfig() {
    prefs.begin("cam", true);
    cfg.wifi_ssid   = prefs.getString("wifi_ssid",   "");
    cfg.wifi_pass   = prefs.getString("wifi_pass",   "");
    cfg.upload_url  = prefs.getString("upload_url",  "");
    cfg.interval_s  = prefs.getUInt  ("interval_s",  300);
    cfg.firebase_api_key = prefs.getString("firebase_api_key", "");
    cfg.firebase_project_id = prefs.getString("firebase_project_id", "");
    cfg.firebase_email = prefs.getString("firebase_email", "");
    cfg.firebase_password = prefs.getString("firebase_password", "");
    prefs.end();

    // ============================================================
    // ⚠️  BENCH ONLY — credenziali hard-coded per il collaudo.
    //     RIMUOVERE QUESTO BLOCCO prima di qualsiasi commit/push
    //     (repo accademico: niente password reali nel versionato).
    //     Sovrascrive la NVS a ogni boot. upload_url lasciato vuoto
    //     finche' la Cloud Function non e' pronta.
    // ============================================================
    cfg.wifi_ssid   = "GiacomoPhone";
    cfg.wifi_pass   = "giacomonoretaaleinternet";
    cfg.firebase_api_key = "" 
    cfg.firebase_project_id = ""
    cfg.firebase_email = ""
    cfg.firebase_password = ""
    
    // ============ fine blocco bench ============
}

/**
 * @brief Salva le configurazioni correnti dell'ESP32-CAM nella memoria NVS.
 */
void saveConfig() {
    prefs.begin("cam", false);
    prefs.putString("wifi_ssid",   cfg.wifi_ssid);
    prefs.putString("wifi_pass",   cfg.wifi_pass);
    prefs.putString("firebase_api_key", cfg.firebase_api_key);
    prefs.putString("firebase_project_id", cfg.firebase_project_id);
    prefs.putString("firebase_email", cfg.firebase_email);
    prefs.putString("firebase_password", cfg.firebase_password);
    prefs.putString("upload_url",  cfg.upload_url);
    prefs.putUInt  ("interval_s",  cfg.interval_s);
    prefs.end();
}

/**
 * @brief Carica le statistiche sull'utilizzo della fotocamera dalla memoria NVS.
 */
void loadStats() {
    statsPrefs.begin("cam_stats", true);
    stats.successful_frames     = statsPrefs.getUInt   ("succ_frames",   0);
    stats.failed_frames         = statsPrefs.getUInt   ("fail_frames",   0);
    stats.upload_errors         = statsPrefs.getUInt   ("upload_err",    0);
    stats.total_capture_time_ms = statsPrefs.getULong64("total_cap_ms",  0);
    statsPrefs.end();
}

void saveStats() {
    statsPrefs.begin("cam_stats", false);
    statsPrefs.putUInt   ("succ_frames",  stats.successful_frames);
    statsPrefs.putUInt   ("fail_frames",  stats.failed_frames);
    statsPrefs.putUInt   ("upload_err",   stats.upload_errors);
    statsPrefs.putULong64("total_cap_ms", stats.total_capture_time_ms);
    statsPrefs.end();
}

void makeDeviceId() {
    // uint8_t mac[6];
    // WiFi.macAddress(mac);
    // snprintf(deviceId, sizeof(deviceId), "%s%02X%02X%02X",
    //          DEVICE_ID_PREFIX, mac[3], mac[4], mac[5]);

    // For now, let deviceId be hardcoded; when we will have more than 1 user, we will derive it from the MAC address.
    snprintf(deviceId, sizeof(deviceId), "CAM_123456");
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
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 30000) {
            Serial.println("[CAM] Wi-Fi connect timeout.");
            return;
        }
        delay(250);
        Serial.print('.');
    }
    Serial.printf("\n[CAM] Wi-Fi OK. IP=%s\n", WiFi.localIP().toString().c_str());
    // NTP: necessario per ottenere timestamp UTC nel payload vision/image.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    // Attesa breve e non-bloccante della sync (max 3 s).
    t0 = millis();
    while (time(nullptr) < NTP_VALID_EPOCH && millis() - t0 < 3000) {
        delay(100);
    }
}

void wifiEnsure() {
    if (cfg.wifi_ssid.length() == 0) return;
    if (WiFi.status() == WL_CONNECTED) return;

    // Se siamo offline, tenta la riconnessione basandosi sul timer
    if (millis() - lastWifiAttemptMs > WIFI_RETRY_INTERVAL_MS || lastWifiAttemptMs == 0) {
        lastWifiAttemptMs = millis();
        Serial.println("[CAM] Wi-Fi disconnesso, tento la riconnessione...");
        WiFi.reconnect();
    }
}

void firebaseInit() {
    if (cfg.firebase_api_key.length() == 0 || app.isInitialized()) return;
    
    Serial.println("[CAM] Inizializzazione Firebase in corso...");
    
    // for now keep insecure to save RAM
    ssl_client.setInsecure(); 
    
    user_auth.api_key = cfg.firebase_api_key;
    user_auth.user.email = cfg.firebase_email;
    user_auth.user.password = cfg.firebase_password;
    
    initializeApp(aClient, app, getAuth(user_auth));
    Serial.println("[CAM] Firebase App Inizializzata.");
}

// TODO: change to on firebase update
// void onMqttMessage(char* topic, byte* payload, unsigned int len) {
//     // Comandi remoti: captureNow, reboot.
//     StaticJsonDocument<256> doc;
//     if (deserializeJson(doc, payload, len)) return;
//     const char* type = doc["type"] | "";
//     if (strcmp(type, "captureNow") == 0) {
//         captureRequested = true; // gestita al prossimo giro di loop
//     } else if (strcmp(type, "reboot") == 0) {
//         ESP.restart();
//     }
// }

String uploadImageToStorage(const uint8_t* buf, size_t len) {
    if (!app.isInitialized()) {
        Serial.println("[CAM] Errore: Firebase non inizializzato prima dell'upload.");
        return "";
    }

    time_t nowEpoch = time(nullptr);
    String filename = "images/" + String(deviceId) + "_" + String(nowEpoch) + ".jpg";
    String bucket = cfg.firebase_project_id + ".appspot.com";
    
    Serial.printf("[CAM] Uploading to Storage: %s...\n", filename.c_str());

    // Wrappa il buffer della fotocamera per la libreria Firebase
    MemoryMedia memoryMedia;
    memoryMedia.data = buf;
    memoryMedia.size = len;
    memoryMedia.mime = "image/jpeg";

    // Esegue l'upload sincrono (bloccante) per garantire che termini prima 
    // di rilasciare il frame buffer
    bool status = CloudStorage::upload(&aClient, bucket.c_str(), filename.c_str(), 
                                       UploadType::MEDIA, memoryMedia, nullptr);

    if (status) {
        // Costruisci e restituisci l'URL in formato gs:// leggibile da Flutter
        String gsUrl = "gs://" + bucket + "/" + filename;
        return gsUrl;
    } else {
        Serial.printf("[CAM] Upload Fallito. Reason: %s\n", aClient.lastError().message().c_str());
        return "";
    }
}

void notifyFirestore(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs) {
    if (!app.isInitialized()) return;

    time_t nowEpoch = time(nullptr);
    // Path basato sulla tua architettura: smartvase/{device_id}/vision/image_ready
    String documentPath = "smartvase/" + String(deviceId) + "/vision/image_ready";

    // Firestore richiede un formato JSON tipizzato
    StaticJsonDocument<1024> doc;
    JsonObject fields = doc.createNestedObject("fields");
    
    fields["timestamp_utc"]["integerValue"] = (nowEpoch > NTP_VALID_EPOCH) ? nowEpoch : 0;
    fields["image_url"]["stringValue"]      = imageUrl;
    fields["resolution"]["stringValue"]     = psramFound() ? "800x600" : "640x480";
    fields["size_bytes"]["integerValue"]    = bytes;
    fields["crc32"]["integerValue"]         = crc;
    fields["capture_time_ms"]["integerValue"] = capMs;
    // fields["plant_healthy"]["booleanValue"] = true;
    // fields["message"]["stringValue"] = "Plant is ok, no need to worry!";

    String payload;
    serializeJson(doc, payload);

    Serial.printf("[CAM] Notifica Firestore su: %s\n", documentPath.c_str());

    // Usa PATCH per creare il documento o fare merge dei campi se esiste già.
    // L'updateMask (ultimo parametro vuoto in questo caso) forza l'aggiornamento.
    bool status = Document::patchDocument(&aClient, cfg.firebase_project_id.c_str(), 
                                          "(default)", documentPath.c_str(), 
                                          payload.c_str(), "");

    if (!status) {
        Serial.printf("[CAM] Errore Firestore: %s\n", aClient.lastError().message().c_str());
        stats.firebase_errors++;
    } else {
        Serial.println("[CAM] Firestore aggiornato con successo.");
    }
}

bool doCapture(bool uploadAndPublish) {
    if (!cameraOk) return false;
    
    unsigned long t0 = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    unsigned long capMs = millis() - t0;
    
    if (!fb) {
        stats.failed_frames++;
        saveStats();
        return false;
    }

    // TODO: Here perform the image analysis
    // AnalysisResult analysis_result = doAnalysis(fb);
    
    size_t frameLen = fb->len;
    uint32_t crc    = crc32_le(0, fb->buf, fb->len);
    stats.successful_frames++;
    stats.total_capture_time_ms += capMs;

    if (uploadAndPublish && cfg.firebase_project_id.length() > 0 && WiFi.status() == WL_CONNECTED) {
        // 1. UPLOAD IMMAGINE
        String url = uploadImageToStorage(fb->buf, frameLen);
        
        // 2. NOTIFICA FIRESTORE (se l'upload è andato a buon fine)
        if (url.length() > 0) {
            // TODO: utilize also analysis_result
            notifyFirestore(url, frameLen, crc, (uint32_t)capMs);
        } else {
            stats.firebase_errors++;
        }
    }
    
    // Libera la memoria del buffer DOPO aver fatto l'upload
    esp_camera_fb_return(fb);
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
    Serial.println("status                Wi-Fi, camera, heap");
    Serial.println("show                  configurazione NVS");
    Serial.println("set <chiave> <val>    wifi_ssid|wifi_pass|");
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
    Serial.printf("upload_url  = %s\n", cfg.upload_url.c_str());
    Serial.printf("interval_s  = %lu\n", (unsigned long)cfg.interval_s);
}

void cliPrintStats() {
    Serial.println("--- stats ---");
    Serial.printf("successful_frames     = %lu\n", (unsigned long)stats.successful_frames);
    Serial.printf("failed_frames         = %lu\n", (unsigned long)stats.failed_frames);
    Serial.printf("upload_errors         = %lu\n", (unsigned long)stats.upload_errors);
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
    if (strcmp(line, "feed")    == 0) {
        return;
    }
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
    if (strncmp(line, "set ", 4) == 0) { 
        cliHandleSet(line + 4); 
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

// -------------------- DEBUG TELEMETRY (monitor seriale) --------------------
#define DEBUG_TELEMETRY_INTERVAL_MS 5000UL

void printDebugTelemetry() {
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    Serial.printf("[DBG] up=%lus | wifi=%s", millis() / 1000UL, wifiUp ? "ON" : "OFF");
    if (wifiUp) Serial.printf(" ip=%s rssi=%ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf(" | heap=%uB | cam=%s | frames ok/fail=%lu/%lu",
                  (unsigned)ESP.getFreeHeap(),
                  cameraOk ? "OK" : "FAIL",
                  (unsigned long)stats.successful_frames,
                  (unsigned long)stats.failed_frames);
    Serial.println();
}

// -------------------- SETUP / LOOP --------------------
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disabilita brown-out (noto issue ESP32-CAM)
    pinMode(3, INPUT_PULLUP); // Evita che RXD0 fluttui quando l'USB/FTDI non e' connesso
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[CAM] SmartVase Vision Co-Processor v" CAM_FW_VERSION);

    loadConfig();
    loadStats();
    makeDeviceId();
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

    firebaseInit();

    Serial.println("[CAM] CLI pronta: digita 'help'");
    Serial.print("> ");
}

void loop() {
    cliTick();

    // Rete in background, mai bloccante per la CLI.
    wifiEnsure();

    // Telemetria di debug periodica sul monitor seriale.
    if (millis() - lastDebugMs >= DEBUG_TELEMETRY_INTERVAL_MS) {
        lastDebugMs = millis();
        printDebugTelemetry();
    }

    // Cattura periodica automatica: solo a catena completa configurata
    // (Wi-Fi connesso + upload_url presente). I test manuali a banco si
    // fanno dalla CLI con 'capture' / 'upload'. captureRequested (firestore)
    // forza una cattura immediata indipendentemente dal timer.
    bool periodicDue = cfg.upload_url.length() > 0 &&
                       WiFi.status() == WL_CONNECTED &&
                       millis() - lastCaptureMs >= (cfg.interval_s * 1000UL);
    if (captureRequested || periodicDue) {
        captureRequested = false;
        doCapture(true);
        lastCaptureMs = millis();
    }

    delay(20);
}
