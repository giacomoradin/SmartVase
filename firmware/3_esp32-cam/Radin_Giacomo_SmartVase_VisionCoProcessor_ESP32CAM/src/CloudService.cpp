#include "CloudService.h"
#include "CameraDriver.h"
#include <time.h>

WiFiClientSecure ssl_client;
AsyncClient aClient(ssl_client);
FirebaseApp app;
UserAuth user_auth("", "", "");
Storage fbStorage;

unsigned long lastCaptureMs     = 0;
unsigned long lastWifiAttemptMs = 0;

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

static void authDebugCallback(AsyncResult &aResult) {
    if (aResult.isError()) {
        Serial.printf("[AUTH ERROR] %s: %s (code %d)\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    }
}

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
    
    Serial.println("[CAM] Firebase App Initialized.");
}

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
                  analysis.plant_healthy,
                  analysis.foliage_coverage * 100.0f,
                  analysis.green_ratio * 100.0f,
                  analysis.brown_ratio * 100.0f,
                  analysis.status_message.c_str());

    if (uploadAndPublish) {
        String gsUrl = uploadImageToStorage(fb->buf, fb->len);
        if (gsUrl.length() > 0) {
            mqttPublishVision(gsUrl, frameLen, crc, capMs, analysis);
        }
    }
    
    esp_camera_fb_return(fb);
    saveStats();
    return true;
}
