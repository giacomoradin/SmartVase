/**
 * @file main.cpp
 * @brief Modular main source file for SmartVase Vision Co-Processor (ESP32-CAM)
 * @author Giacomo Radin
 * @date 2026-07-03
 */

#include <Arduino.h>
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "ConfigManager.h"
#include "CameraDriver.h"
#include "VisionBotanist.h"
#include "CloudService.h"
#include "ConsoleCLI.h"

//flag tracking manual photo capture requests from app or cli
bool captureRequested = false;
static unsigned long lastDebugMs = 0;
#define DEBUG_TELEMETRY_INTERVAL_MS 5000UL

//print periodic status string to serial console for debugging
static void printDebugTelemetry() {
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    Serial.printf("[DBG] up=%lus | wifi=%s", millis() / 1000UL, wifiUp ? "ON" : "OFF");
    if (wifiUp) Serial.printf(" ip=%s rssi=%ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf(" | heap=%uB | cam=%s | frames ok/fail=%lu/%lu\n",
                  (unsigned)ESP.getFreeHeap(),
                  cameraOk ? "OK" : "FAIL",
                  (unsigned long)stats.successful_frames,
                  (unsigned long)stats.failed_frames);
}

//hardware initialization and service startup routine
void setup() {
    //disable brownout detector to prevent resets during camera power surge
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[CAM] SmartVase Vision Co-Processor v" CAM_FW_VERSION);

    //load saved configuration and operating statistics from non-volatile memory
    loadConfig();
    loadStats();
    makeDeviceId();
    Serial.printf("[CAM] device_id=%s\n", deviceId);

    //initialize camera sensor hardware and check status
    cameraOk = initCamera();
    if (!cameraOk) {
        Serial.println("[CAM] Camera init FAILED. CLI remains active for debugging.");
    }

    //attempt wifi station connection if network credentials exist
    if (cfg.wifi_ssid.length() > 0) {
        wifiStartAttempt();
    } else {
        Serial.println("[CAM] Wi-Fi unconfigured. From CLI: set wifi_ssid <...>, set wifi_pass <...>, save, reboot");
    }

    //initialize firebase cloud client services
    firebaseInit();

    Serial.println("[CAM] CLI ready: type 'help'");
    Serial.print("> ");
}

//main execution loop running continuously
void loop() {
    //handle interactive serial terminal input
    cliTick();
    //check and maintain wifi network connection
    wifiEnsure();
    //process async firebase client background tasks
    app.loop();
    //poll firestore for on demand capture requests from app
    checkCommand();

    //print debug telemetry string periodically
    if (millis() - lastDebugMs >= DEBUG_TELEMETRY_INTERVAL_MS) {
        lastDebugMs = millis();
        printDebugTelemetry();
    }

    //check if periodic timer expired and cloud upload is configured
    bool periodicDue = (cfg.firebase_project_id.length() > 0 || cfg.upload_url.length() > 0) &&
                       WiFi.status() == WL_CONNECTED &&
                       millis() - lastCaptureMs >= (cfg.interval_s * 1000UL);
    //execute photo capture if requested by app or triggered by timer
    if (captureRequested || periodicDue) {
        captureRequested = false;
        doCapture(true);
        //reset timer timestamp after capture completes
        lastCaptureMs = millis();
    }

    delay(20);
}
