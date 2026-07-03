/**
 * @file main.cpp
 * @brief Main source file for SmartVase Vision Co-Processor (ESP32-CAM)
 * @author Giacomo Radin
 * @date 2026-06-30
 */

/*
 * =================================================================
 * SmartVase - Vision Co-Processor (ESP32-CAM)
 * Version 2.2 — 2026-07-02 (Zenithal HSV + Circular ROI)
 * =================================================================
 *  Architecture:
 *   - Autonomous STA Wi-Fi (credentials stored in NVS via Preferences),
 *     non-blocking reconnection with background retry.
 *   - Serial CLI for provisioning and bench testing (115200, 'help').
 *   - Periodic JPEG capture when network and Firebase are configured.
 *   - Direct upload to Firebase Storage using mobizt/FirebaseClient v2.
 *   - Onboard plant health analysis (PixelAnalyzer) using PSRAM buffer:
 *     - Converts decoded RGB888 pixels inside Circular ROI into HSV space.
 *     - Measures foliage coverage ratio to detect leaf wilting/shrinking.
 *     - Classifies healthy green vs yellow/brown chlorosis/necrosis.
 *   - Firestore update on smartvase/{device_id}/vision/latest with
 *     image URL, plant health classification, ratios, coverage, and CRC32.
 *   - Cumulative stats persisted in NVS.
 *
 *  Configuration (NVS namespace "cam", writable from CLI via `set`):
 *     wifi_ssid           string
 *     wifi_pass           string
 *     firebase_api_key    string
 *     firebase_project_id string
 *     firebase_email      string
 *     firebase_password   string
 *     upload_url          string
 *     interval_s          uint32  (default 300)
 *     roi_center_x        uint16  (default 400 for SVGA center)
 *     roi_center_y        uint16  (default 300 for SVGA center)
 *     roi_radius          uint16  (default 280, covers pot + overflow)
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
#include <math.h>
#include "secrets.h"


#define ENABLE_USER_AUTH
#define ENABLE_STORAGE
#define ENABLE_FIRESTORE
#include <FirebaseClient.h>

#define CAM_FW_VERSION "2.2.0"

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
#define WIFI_RETRY_INTERVAL_MS   30000UL  // STA reconnect attempt every 30 s
#define NTP_VALID_EPOCH      1700000000UL // below this threshold epoch time is not synchronized

// -------------------- NVS CONFIG --------------------
struct CamConfig {
    String wifi_ssid;
    String wifi_pass;
    String firebase_api_key;
    String firebase_project_id;
    String firebase_email;
    String firebase_password;
    String upload_url;
    uint32_t interval_s;
    uint16_t roi_center_x;
    uint16_t roi_center_y;
    uint16_t roi_radius;
} cfg;

// -------------------- FIREBASE GLOBALS --------------------
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

FirebaseApp app;
UserAuth user_auth("", "", "");
Storage fbStorage;
Firestore::Documents Docs;

// -------------------- NVS PREFERENCES --------------------
Preferences prefs;
Preferences statsPrefs;

struct CamStats {
    uint32_t successful_frames;
    uint32_t failed_frames;
    uint32_t upload_errors;
    uint32_t firebase_errors;
    uint64_t total_capture_time_ms;
} stats;

// -------------------- STATE --------------------
unsigned long lastCaptureMs     = 0;
unsigned long lastWifiAttemptMs = 0;
bool cameraOk                   = false;
bool captureRequested           = false; // captureNow requested remotely
unsigned long lastDebugMs       = 0;     // throttle debug serial telemetry

// -------------------- PLANT HEALTH ANALYSIS RESULT --------------------
struct AnalysisResult {
    bool valid;
    uint32_t green_pixels;
    uint32_t brown_pixels;
    uint32_t total_roi_pixels;
    float green_ratio;
    float brown_ratio;
    float foliage_coverage;
    bool plant_healthy;
    String status_message;
};

// -------------------- UTILITIES --------------------
/**
 * @brief Computes CRC32 checksum (Little Endian) of a data buffer.
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
 * @brief Loads ESP32-CAM configuration from NVS memory.
 */
void loadConfig() {
    prefs.begin("cam", true);
    cfg.wifi_ssid           = prefs.getString("wifi_ssid",           "");
    cfg.wifi_pass           = prefs.getString("wifi_pass",           "");
    cfg.upload_url          = prefs.getString("upload_url",          "");
    cfg.interval_s          = prefs.getUInt  ("interval_s",          300);
    cfg.firebase_api_key    = prefs.getString("firebase_api_key",    "");
    cfg.firebase_project_id = prefs.getString("firebase_project_id", "");
    cfg.firebase_email      = prefs.getString("firebase_email",      "");
    cfg.firebase_password   = prefs.getString("firebase_password",   "");
    cfg.roi_center_x        = prefs.getUInt  ("roi_cx",              400);
    cfg.roi_center_y        = prefs.getUInt  ("roi_cy",              300);
    cfg.roi_radius          = prefs.getUInt  ("roi_r",               280);
    prefs.end();

    // ============================================================
    // BENCH / SECRETS — Load credentials from secrets.h
    // ============================================================
#ifdef SMARTVASE_CAM_SECRETS_H
    cfg.wifi_ssid           = SECRET_WIFI_SSID;
    cfg.wifi_pass           = SECRET_WIFI_PASS;
    cfg.firebase_api_key    = SECRET_FIREBASE_API_KEY;
    cfg.firebase_project_id = SECRET_FIREBASE_PROJECT_ID;
    cfg.firebase_email      = SECRET_FIREBASE_EMAIL;
    cfg.firebase_password   = SECRET_FIREBASE_PASSWORD;
#else
    cfg.wifi_ssid   = "XXL";
    cfg.wifi_pass   = "pomodoro";
#endif
    // ============ End bench block ============
}

/**
 * @brief Saves current ESP32-CAM configuration to NVS memory.
 */
void saveConfig() {
    prefs.begin("cam", false);
    prefs.putString("wifi_ssid",           cfg.wifi_ssid);
    prefs.putString("wifi_pass",           cfg.wifi_pass);
    prefs.putString("firebase_api_key",    cfg.firebase_api_key);
    prefs.putString("firebase_project_id", cfg.firebase_project_id);
    prefs.putString("firebase_email",      cfg.firebase_email);
    prefs.putString("firebase_password",   cfg.firebase_password);
    prefs.putString("upload_url",          cfg.upload_url);
    prefs.putUInt  ("interval_s",          cfg.interval_s);
    prefs.putUInt  ("roi_cx",              cfg.roi_center_x);
    prefs.putUInt  ("roi_cy",              cfg.roi_center_y);
    prefs.putUInt  ("roi_r",               cfg.roi_radius);
    prefs.end();
}

/**
 * @brief Loads camera operational statistics from NVS memory.
 */
void loadStats() {
    statsPrefs.begin("cam_stats", true);
    stats.successful_frames     = statsPrefs.getUInt   ("succ_frames",  0);
    stats.failed_frames         = statsPrefs.getUInt   ("fail_frames",  0);
    stats.upload_errors         = statsPrefs.getUInt   ("upload_err",   0);
    stats.firebase_errors       = statsPrefs.getUInt   ("fb_err",       0);
    stats.total_capture_time_ms = statsPrefs.getULong64("total_cap_ms", 0);
    statsPrefs.end();
}

/**
 * @brief Saves camera operational statistics to NVS memory.
 */
void saveStats() {
    statsPrefs.begin("cam_stats", false);
    statsPrefs.putUInt   ("succ_frames",  stats.successful_frames);
    statsPrefs.putUInt   ("fail_frames",  stats.failed_frames);
    statsPrefs.putUInt   ("upload_err",   stats.upload_errors);
    statsPrefs.putUInt   ("fb_err",       stats.firebase_errors);
    statsPrefs.putULong64("total_cap_ms", stats.total_capture_time_ms);
    statsPrefs.end();
}

void makeDeviceId() {
    snprintf(deviceId, sizeof(deviceId), "CAM_123456");
}

// -------------------- CAMERA INITIALIZATION --------------------
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
        c.frame_size   = FRAMESIZE_SVGA;  // 800x600 resolution
        c.jpeg_quality = 12;
        c.fb_count     = 2;
    } else {
        c.frame_size   = FRAMESIZE_VGA;   // 640x480 resolution
        c.jpeg_quality = 14;
        c.fb_count     = 1;
    }
    esp_err_t err = esp_camera_init(&c);
    return err == ESP_OK;
}

// -------------------- WIFI (NON-BLOCKING) --------------------
void wifiStartAttempt() {
    if (cfg.wifi_ssid.length() == 0) return;
    Serial.printf("[CAM] Wi-Fi: attempting connection to '%s'...\n", cfg.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 30000) {
            Serial.println("[CAM] Wi-Fi connection timeout.");
            return;
        }
        delay(250);
        Serial.print('.');
    }
    Serial.printf("\n[CAM] Wi-Fi OK. IP=%s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    t0 = millis();
    while (time(nullptr) < NTP_VALID_EPOCH && millis() - t0 < 5000) {
        delay(100);
    }
}

void wifiEnsure() {
    if (cfg.wifi_ssid.length() == 0) return;
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiAttemptMs > WIFI_RETRY_INTERVAL_MS || lastWifiAttemptMs == 0) {
            lastWifiAttemptMs = millis();
            Serial.println("[CAM] Wi-Fi disconnected, attempting reconnection...");
            WiFi.reconnect();
        }
    } else {
        if (time(nullptr) < NTP_VALID_EPOCH) {
            configTime(0, 0, "pool.ntp.org", "time.google.com");
        }
    }
}

void authDebugCallback(AsyncResult &aResult) {
    if (aResult.isError()) {
        Serial.printf("[AUTH ERROR] %s: %s (code %d)\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    }
}

// -------------------- FIREBASE INITIALIZATION --------------------
void firebaseInit() {
    if (cfg.firebase_api_key.length() == 0 || app.isInitialized()) return;
    
    if (time(nullptr) < NTP_VALID_EPOCH) {
        Serial.println("[CAM] Waiting for NTP time sync before Firebase init...");
        unsigned long t0 = millis();
        while (time(nullptr) < NTP_VALID_EPOCH && millis() - t0 < 5000) {
            delay(100);
        }
    }
    
    Serial.println("[CAM] Initializing Firebase...");
    ssl_client.setInsecure(); 
    
    user_auth = UserAuth(cfg.firebase_api_key, cfg.firebase_email, cfg.firebase_password);
    initializeApp(aClient, app, getAuth(user_auth), authDebugCallback, "auth_task");
    
    app.getApp<Storage>(fbStorage);
    app.getApp<Firestore::Documents>(Docs);
    
    Serial.println("[CAM] Firebase App Initialized.");
}

// -------------------- ONBOARD HSV & CIRCULAR ROI ANALYSIS --------------------
/**
 * @brief Analyzes image inside Circular ROI using HSV space to classify health and wilting.
 */
AnalysisResult doAnalysis(camera_fb_t* fb) {
    AnalysisResult res = {false, 0, 0, 0, 0.0f, 0.0f, 0.0f, true, "Cannot analyze frame (invalid buffer)"};
    if (!fb || fb->len == 0) return res;

    uint8_t* rgb_buf = (uint8_t*)heap_caps_malloc(fb->width * fb->height * 3, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        res.status_message = "Insufficient PSRAM memory for RGB decoding";
        return res;
    }

    bool decoded = fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
    if (decoded) {
        uint32_t cx = cfg.roi_center_x;
        uint32_t cy = cfg.roi_center_y;
        uint32_t r_sq = (uint32_t)cfg.roi_radius * (uint32_t)cfg.roi_radius;

        for (uint32_t y = 0; y < fb->height; y++) {
            uint32_t dy = (y > cy) ? (y - cy) : (cy - y);
            for (uint32_t x = 0; x < fb->width; x++) {
                uint32_t dx = (x > cx) ? (x - cx) : (cx - x);
                if (dx * dx + dy * dy > r_sq) {
                    continue; // Skip pixels outside the configured circular ROI
                }

                res.total_roi_pixels++;

                uint32_t idx = (y * fb->width + x) * 3;
                float rf = rgb_buf[idx] / 255.0f;
                float gf = rgb_buf[idx+1] / 255.0f;
                float bf = rgb_buf[idx+2] / 255.0f;

                float cmax = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
                float cmin = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
                float delta = cmax - cmin;

                float h = 0.0f;
                if (delta > 0.0001f) {
                    if (cmax == rf)      h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
                    else if (cmax == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
                    else                 h = 60.0f * (((rf - gf) / delta) + 4.0f);
                    if (h < 0.0f) h += 360.0f;
                }
                float s = (cmax > 0.0001f) ? (delta / cmax) : 0.0f;
                float v = cmax;

                // Healthy green basil foliage (Hue ~35..95 deg, sufficient color & brightness)
                if (h >= 35.0f && h <= 95.0f && s >= 0.20f && v >= 0.15f) {
                    res.green_pixels++;
                }
                // Yellow/brown/dry foliage (Hue ~10..35 deg)
                else if (h >= 10.0f && h < 35.0f && s >= 0.25f && v >= 0.15f) {
                    res.brown_pixels++;
                }
            }
        }

        if (res.total_roi_pixels > 0) {
            res.green_ratio = (float)res.green_pixels / res.total_roi_pixels;
            res.brown_ratio = (float)res.brown_pixels / res.total_roi_pixels;
            res.foliage_coverage = (float)(res.green_pixels + res.brown_pixels) / res.total_roi_pixels;
            res.valid = true;

            if (res.foliage_coverage < 0.05f) {
                res.plant_healthy = false;
                res.status_message = "Warning: Very low foliage coverage (possible wilting or missing plant)!";
            } else if (res.brown_pixels > res.green_pixels * 0.25f) {
                res.plant_healthy = false;
                res.status_message = "Warning: Detected dry, yellowing, or sick foliage!";
            } else {
                res.plant_healthy = true;
                res.status_message = "Healthy: Basil plant has good turgor and green foliage!";
            }
        } else {
            res.status_message = "Invalid circular ROI (0 pixels evaluated)";
        }
    } else {
        res.status_message = "JPEG to RGB888 frame decoding failed";
    }

    heap_caps_free(rgb_buf);
    return res;
}

// -------------------- FIREBASE STORAGE & FIRESTORE --------------------
String uploadImageToStorage(const uint8_t* buf, size_t len) {
    if (!app.ready()) {
        Serial.println("[CAM] Error: Firebase is not ready for upload.");
        return "";
    }

    if (time(nullptr) < NTP_VALID_EPOCH) {
        Serial.println("[CAM] Waiting for NTP time sync before upload...");
        unsigned long t0 = millis();
        while (time(nullptr) < NTP_VALID_EPOCH && millis() - t0 < 5000) {
            delay(100);
        }
    }

    time_t nowEpoch = time(nullptr);
    String filename = "images/" + String(deviceId) + "_" + String(nowEpoch) + ".jpg";
    String bucket = cfg.firebase_project_id + ".firebasestorage.app";

    Serial.printf("[CAM] Uploading to Storage: %s...\n", filename.c_str());

    BlobConfig blob((uint8_t*)buf, len);

    bool status = fbStorage.upload(aClient, FirebaseStorage::Parent(bucket, filename), getBlob(blob), "image/jpeg");

    if (status) {
        String gsUrl = "gs://" + bucket + "/" + filename;
        Serial.printf("[CAM] Upload completed successfully: %s\n", gsUrl.c_str());
        return gsUrl;
    } else {
        stats.upload_errors++;
        Serial.printf("[CAM] Upload failed. Error: %s (code %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
        return "";
    }
}

void notifyFirestore(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs, const AnalysisResult& analysis) {
    if (!app.ready()) return;

    time_t nowEpoch = time(nullptr);
    String documentPath = "smartvase/" + String(deviceId) + "/vision/latest";

    Values::IntegerValue tsVal((nowEpoch > NTP_VALID_EPOCH) ? nowEpoch : 0);
    Values::StringValue  urlVal(imageUrl);
    Values::StringValue  resVal(psramFound() ? "800x600" : "640x480");
    Values::IntegerValue sizeVal(bytes);
    Values::IntegerValue crcVal(crc);
    Values::IntegerValue capVal(capMs);
    Values::BooleanValue healthVal(analysis.plant_healthy);
    Values::StringValue  msgVal(analysis.status_message);
    Values::StringValue  covVal(String(analysis.foliage_coverage * 100.0f, 1) + "%");
    Values::StringValue  grnVal(String(analysis.green_ratio * 100.0f, 1) + "%");
    Values::StringValue  brnVal(String(analysis.brown_ratio * 100.0f, 1) + "%");

    Document<Values::Value> doc("timestamp_utc", Values::Value(tsVal));
    doc.add("image_url",        Values::Value(urlVal));
    doc.add("resolution",       Values::Value(resVal));
    doc.add("size_bytes",       Values::Value(sizeVal));
    doc.add("crc32",            Values::Value(crcVal));
    doc.add("capture_time_ms",  Values::Value(capVal));
    doc.add("plant_healthy",    Values::Value(healthVal));
    doc.add("status_message",   Values::Value(msgVal));
    doc.add("foliage_coverage", Values::Value(covVal));
    doc.add("green_ratio",      Values::Value(grnVal));
    doc.add("brown_ratio",      Values::Value(brnVal));

    Serial.printf("[CAM] Updating Firestore document at: %s\n", documentPath.c_str());

    DocumentMask updateMask;
    DocumentMask mask;
    Precondition precondition;
    PatchDocumentOptions patchOptions(updateMask, mask, precondition);
    Docs.patch(aClient, Firestore::Parent(cfg.firebase_project_id), documentPath, patchOptions, doc);

    if (aClient.lastError().code() == 0) {
        Serial.println("[CAM] Firestore document updated successfully.");
    } else {
        Serial.printf("[CAM] Firestore update failed: %s\n", aClient.lastError().message().c_str());
        stats.firebase_errors++;
    }
}

// -------------------- IMAGE CAPTURE ROUTINE --------------------
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

    size_t frameLen = fb->len;
    uint32_t crc    = crc32_le(0, fb->buf, fb->len);
    stats.successful_frames++;
    stats.total_capture_time_ms += capMs;

    Serial.println("[CAM] Performing onboard HSV & Circular ROI foliage analysis...");
    AnalysisResult analysis = doAnalysis(fb);
    Serial.printf("[CAM] Analysis result: Healthy=%d | Coverage=%.1f%% | Green=%.1f%% | Brown=%.1f%% | Msg=%s\n",
                  analysis.plant_healthy, analysis.foliage_coverage * 100.0f,
                  analysis.green_ratio * 100.0f, analysis.brown_ratio * 100.0f,
                  analysis.status_message.c_str());

    if (uploadAndPublish && cfg.firebase_project_id.length() > 0 && WiFi.status() == WL_CONNECTED) {
        String url = uploadImageToStorage(fb->buf, frameLen);
        if (url.length() > 0) {
            notifyFirestore(url, frameLen, crc, (uint32_t)capMs, analysis);
        }
    }
    
    esp_camera_fb_return(fb);
    saveStats();
    return true;
}

// -------------------- SERIAL CLI --------------------
static char cliBuf[192];
static size_t cliPos = 0;

void cliPrintHelp() {
    Serial.println("--- SmartVase CAM CLI v" CAM_FW_VERSION " ---");
    Serial.println("help                  show this menu");
    Serial.println("version               firmware version");
    Serial.println("status                Wi-Fi, camera, free heap");
    Serial.println("show                  show NVS config & circular ROI settings");
    Serial.println("set <key> <val>       wifi_ssid|wifi_pass|firebase_project_id|firebase_api_key|roi_center_x|roi_center_y|roi_radius");
    Serial.println("save                  save current config to NVS");
    Serial.println("wifi connect          trigger STA Wi-Fi connection attempt");
    Serial.println("capture               test capture & onboard HSV circular ROI analysis");
    Serial.println("upload                capture + analysis + upload + Firestore update");
    Serial.println("stats                 show cumulative statistics");
    Serial.println("reboot                reboot the camera module");
}

void cliPrintStatus() {
    Serial.println("--- status ---");
    Serial.printf("fw_version = %s\n", CAM_FW_VERSION);
    Serial.printf("device_id  = %s\n", deviceId);
    Serial.printf("camera     = %s\n", cameraOk ? "OK" : "FAILED");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi       = CONNECTED ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.printf("wifi       = OFFLINE%s\n",
                      cfg.wifi_ssid.length() == 0 ? " (unconfigured)" : "");
    }
    time_t nowEpoch = time(nullptr);
    Serial.printf("ntp_epoch  = %lu%s\n", (unsigned long)nowEpoch,
                  nowEpoch < (time_t)NTP_VALID_EPOCH ? " (unsynchronized)" : "");
    Serial.printf("free_heap  = %u B\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("uptime_s   = %lu\n", millis() / 1000UL);
}

void cliPrintShow() {
    Serial.println("--- NVS config ---");
    Serial.printf("wifi_ssid           = %s\n", cfg.wifi_ssid.c_str());
    Serial.printf("wifi_pass           = %s\n", cfg.wifi_pass.length() ? "***" : "(empty)");
    Serial.printf("firebase_api_key    = %s\n", cfg.firebase_api_key.length() ? "***" : "(empty)");
    Serial.printf("firebase_project_id = %s\n", cfg.firebase_project_id.c_str());
    Serial.printf("upload_url          = %s\n", cfg.upload_url.c_str());
    Serial.printf("interval_s          = %lu\n", (unsigned long)cfg.interval_s);
    Serial.printf("roi_center_x        = %u\n", (unsigned)cfg.roi_center_x);
    Serial.printf("roi_center_y        = %u\n", (unsigned)cfg.roi_center_y);
    Serial.printf("roi_radius          = %u\n", (unsigned)cfg.roi_radius);
}

void cliPrintStats() {
    Serial.println("--- statistics ---");
    Serial.printf("successful_frames     = %lu\n", (unsigned long)stats.successful_frames);
    Serial.printf("failed_frames         = %lu\n", (unsigned long)stats.failed_frames);
    Serial.printf("upload_errors         = %lu\n", (unsigned long)stats.upload_errors);
    Serial.printf("firebase_errors       = %lu\n", (unsigned long)stats.firebase_errors);
    Serial.printf("total_capture_time_ms = %llu\n", stats.total_capture_time_ms);
}

bool cliHandleSet(char* args) {
    char* space = strchr(args, ' ');
    if (space == nullptr) return false;
    *space = '\0';
    const char* key   = args;
    const char* value = space + 1;
    if (strlen(value) == 0) return false;

    if      (strcmp(key, "wifi_ssid")           == 0) cfg.wifi_ssid           = value;
    else if (strcmp(key, "wifi_pass")           == 0) cfg.wifi_pass           = value;
    else if (strcmp(key, "firebase_api_key")    == 0) cfg.firebase_api_key    = value;
    else if (strcmp(key, "firebase_project_id") == 0) cfg.firebase_project_id = value;
    else if (strcmp(key, "firebase_email")      == 0) cfg.firebase_email      = value;
    else if (strcmp(key, "firebase_password")   == 0) cfg.firebase_password   = value;
    else if (strcmp(key, "upload_url")          == 0) cfg.upload_url          = value;
    else if (strcmp(key, "interval_s")          == 0) {
        long s = atol(value);
        if (s < 10) { Serial.println("[CLI] minimum interval is 10 s"); return true; }
        cfg.interval_s = (uint32_t)s;
    }
    else if (strcmp(key, "roi_center_x")        == 0) cfg.roi_center_x = (uint16_t)atoi(value);
    else if (strcmp(key, "roi_center_y")        == 0) cfg.roi_center_y = (uint16_t)atoi(value);
    else if (strcmp(key, "roi_radius")          == 0) cfg.roi_radius   = (uint16_t)atoi(value);
    else return false;

    Serial.printf("[CLI] %s set successfully (remember to 'save' and 'reboot')\n", key);
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
    if (strcmp(line, "feed")    == 0) { return; }
    if (strcmp(line, "capture") == 0) { doCapture(false); return; }
    if (strcmp(line, "upload")  == 0) {
        if (cfg.firebase_project_id.length() == 0 && cfg.upload_url.length() == 0) {
            Serial.println("[CLI] Neither firebase_project_id nor upload_url configured");
        } else if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[CLI] Wi-Fi offline");
        } else {
            doCapture(true);
        }
        return;
    }
    if (strcmp(line, "save") == 0) {
        saveConfig();
        Serial.println("[CLI] configuration saved to NVS (reboot required with 'reboot')");
        return;
    }
    if (strcmp(line, "wifi connect") == 0) {
        if (cfg.wifi_ssid.length() == 0) Serial.println("[CLI] wifi_ssid not configured");
        else wifiStartAttempt();
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        Serial.println("[CLI] rebooting...");
        delay(200);
        ESP.restart();
        return;
    }
    if (strncmp(line, "set ", 4) == 0) { 
        cliHandleSet(line + 4); 
        return; 
    }
    Serial.printf("[CLI] unknown command: '%s' (try 'help')\n", line);
}

void cliTick() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        Serial.write(c); // Hardware echo
        if (c == '\r' || c == '\n') {
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
            Serial.println("[CLI] line too long, discarded");
        }
    }
}

// -------------------- DEBUG TELEMETRY (Serial Monitor) --------------------
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
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[CAM] SmartVase Vision Co-Processor v" CAM_FW_VERSION);

    loadConfig();
    loadStats();
    makeDeviceId();
    Serial.printf("[CAM] device_id=%s\n", deviceId);

    cameraOk = initCamera();
    if (!cameraOk) {
        Serial.println("[CAM] Camera init FAILED. CLI remains active for debugging.");
    }

    if (cfg.wifi_ssid.length() > 0) {
        wifiStartAttempt();
    } else {
        Serial.println("[CAM] Wi-Fi unconfigured. From CLI: set wifi_ssid <...>, set wifi_pass <...>, save, reboot");
    }

    firebaseInit();

    Serial.println("[CAM] CLI ready: type 'help'");
    Serial.print("> ");
}

void loop() {
    cliTick();
    wifiEnsure();
    app.loop();

    if (millis() - lastDebugMs >= DEBUG_TELEMETRY_INTERVAL_MS) {
        lastDebugMs = millis();
        printDebugTelemetry();
    }

    bool periodicDue = (cfg.firebase_project_id.length() > 0 || cfg.upload_url.length() > 0) &&
                       WiFi.status() == WL_CONNECTED &&
                       millis() - lastCaptureMs >= (cfg.interval_s * 1000UL);
    if (captureRequested || periodicDue) {
        captureRequested = false;
        doCapture(true);
        lastCaptureMs = millis();
    }

    delay(20);
}
