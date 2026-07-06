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

//global network and firebase service client instances
extern WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
extern AsyncClient aClient;
extern FirebaseApp app;
extern UserAuth user_auth;
extern Storage fbStorage;
extern Firestore::Documents Docs;

//timestamps tracking previous camera captures and wifi attempts
extern unsigned long lastCaptureMs;
extern unsigned long lastWifiAttemptMs;
//flag indicating whether app requested an on demand capture
extern bool captureRequested;

//retry interval threshold between wifi reconnection attempts
#define WIFI_RETRY_INTERVAL_MS 30000UL

//start asynchronous wifi station connection attempt
void wifiStartAttempt();
//check wifi connectivity and reconnect if connection dropped
void wifiEnsure();
//configure firebase authentication and storage clients
void firebaseInit();
//calculate crc32 checksum for data verification
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
//upload jpeg frame buffer to firebase cloud storage bucket
String uploadImageToStorage(const uint8_t* buf, size_t len);
//update firestore database with image url and analysis results
void notifyFirestore(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs, const AnalysisResult& analysis);
//capture camera frame and optionally upload to cloud
bool doCapture(bool uploadAndPublish);
//poll firestore database for on demand capture commands
void checkCommand();

#endif // CLOUD_SERVICE_H
