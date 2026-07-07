#ifndef CLOUD_SERVICE_H
#define CLOUD_SERVICE_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define ENABLE_USER_AUTH
#define ENABLE_STORAGE
#include <FirebaseClient.h>

#include "ConfigManager.h"
#include "VisionBotanist.h"
#include "CamMqtt.h"

extern WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
extern AsyncClient aClient;
extern FirebaseApp app;
extern UserAuth user_auth;
extern Storage fbStorage;

extern unsigned long lastCaptureMs;
extern unsigned long lastWifiAttemptMs;

#define WIFI_RETRY_INTERVAL_MS 30000UL

void wifiStartAttempt();
void wifiEnsure();
void firebaseInit();
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
String uploadImageToStorage(const uint8_t* buf, size_t len);
bool doCapture(bool uploadAndPublish);

#endif // CLOUD_SERVICE_H
