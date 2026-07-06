#include "CloudService.h"
#include "CameraDriver.h"
#include <time.h>

WiFiClientSecure ssl_client;
AsyncClient aClient(ssl_client);
FirebaseApp app;
UserAuth user_auth("", "", "");
Storage fbStorage;
Firestore::Documents Docs;

unsigned long lastCaptureMs     = 0;
unsigned long lastWifiAttemptMs = 0;

//calculate crc32 checksum using lookup table for fast bitwise computation
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

//start asynchronous station mode wifi connection and sync ntp time
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

//check wifi status periodically and trigger reconnection if disconnected
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

//initialize firebase storage and firestore service connections
void firebaseInit() {
    if (cfg.firebase_api_key.length() == 0 || app.isInitialized()) return;
    
    //wait for valid ntp time timestamp before connecting to ssl endpoints
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

//upload captured image buffer to firebase cloud storage bucket
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

//patch firestore vision latest document with image url and health telemetry
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

//acquire camera frame perform foliage analysis and optionally upload to cloud
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
            notifyFirestore(gsUrl, frameLen, crc, capMs, analysis);
        }
    }
    
    //return camera frame buffer to sensor memory pool
    esp_camera_fb_return(fb);
    saveStats();
    return true;
}

//extract integer value string from firestore json response
static long extractInteger(const String& json, const char* key) {
    int keyPos = json.indexOf(key);
    if (keyPos == -1) return -1;
    int valPos = json.indexOf("\"integerValue\":", keyPos);
    if (valPos == -1) valPos = json.indexOf("\"stringValue\":", keyPos);
    if (valPos == -1) return -1;
    int quote1 = json.indexOf('"', valPos + 14);
    if (quote1 == -1) return -1;
    int quote2 = json.indexOf('"', quote1 + 1);
    if (quote2 == -1) return -1;
    String numStr = json.substring(quote1 + 1, quote2);
    return numStr.toInt();
}

//handle async firestore response for on demand capture command check
static void commandCallback(AsyncResult &aResult) {
    if (!aResult.isResult()) return;
    if (aResult.isError()) {
        //ignore 404 error if command document does not exist yet
        if (aResult.error().code() != 404 && aResult.error().code() != 0) {
            Serial.printf("[CAM] Command check error: %s (code %d)\n", aResult.error().message().c_str(), aResult.error().code());
        }
        return;
    }
    if (aResult.available()) {
        String payload = aResult.c_str();
        long cmdId = extractInteger(payload, "\"cmd_id\"");
        long tsUtc = extractInteger(payload, "\"timestamp_utc\"");
        
        static long lastProcessedCmdId = -1;
        static long lastProcessedTs = -1;
        static bool firstCommandRead = false;

        //sync with existing database command on first boot without taking photo
        if (!firstCommandRead) {
            firstCommandRead = true;
            if (cmdId != -1) lastProcessedCmdId = cmdId;
            if (tsUtc != -1) lastProcessedTs = tsUtc;
            Serial.printf("[CAM] Initial command sync: cmd_id=%ld, ts=%ld\n", lastProcessedCmdId, lastProcessedTs);
            return;
        }

        bool newCmd = false;
        //check if command id or timestamp increased compared to last execution
        if (cmdId != -1 && cmdId > lastProcessedCmdId) {
            newCmd = true;
            lastProcessedCmdId = cmdId;
        }
        if (tsUtc != -1 && tsUtc > lastProcessedTs) {
            newCmd = true;
            lastProcessedTs = tsUtc;
        }

        //trigger instant capture if new command detected
        if (newCmd) {
            Serial.printf("[CAM] New on-demand capture command received! (cmd_id=%ld, ts=%ld)\n", lastProcessedCmdId, lastProcessedTs);
            captureRequested = true;
        }
    }
}

//poll firestore database periodically for instant capture requests
void checkCommand() {
    if (!app.ready() || WiFi.status() != WL_CONNECTED) return;
    if (cfg.firebase_project_id.length() == 0) return;
    
    static unsigned long lastCheckMs = 0;
    if (millis() - lastCheckMs < 5000UL) return;
    lastCheckMs = millis();

    String documentPath = "smartvase/" + String(deviceId) + "/command/capture";
    Docs.get(aClient, Firestore::Parent(cfg.firebase_project_id), documentPath, GetDocumentOptions(), commandCallback, "cmd_check");
}
