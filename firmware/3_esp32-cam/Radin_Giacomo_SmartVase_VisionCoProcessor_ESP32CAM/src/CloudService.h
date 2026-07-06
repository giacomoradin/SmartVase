#ifndef CLOUD_SERVICE_H
#define CLOUD_SERVICE_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define ENABLE_USER_AUTH
#define ENABLE_STORAGE
#define ENABLE_FIRESTORE
#include <FirebaseClient.h>

#include "ConfigManager.h"
#include "VisionBotanist.h"

extern WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
extern AsyncClient aClient;
extern FirebaseApp app;
extern UserAuth user_auth;
extern Storage fbStorage;
extern Firestore::Documents Docs;

extern unsigned long lastCaptureMs;
extern unsigned long lastWifiAttemptMs;
extern bool captureRequested;

#define WIFI_RETRY_INTERVAL_MS 30000UL

void wifiStartAttempt();
void wifiEnsure();
void firebaseInit();
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
String uploadImageToStorage(const uint8_t* buf, size_t len);
void notifyFirestore(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs, const AnalysisResult& analysis);
bool doCapture(bool uploadAndPublish);
void checkCommand();

#endif // CLOUD_SERVICE_H
