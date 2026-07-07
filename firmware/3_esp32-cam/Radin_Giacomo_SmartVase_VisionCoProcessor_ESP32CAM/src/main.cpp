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

bool captureRequested = false;
static unsigned long lastDebugMs = 0;
#define DEBUG_TELEMETRY_INTERVAL_MS 5000UL

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
    mqttInit();

    Serial.println("[CAM] CLI ready: type 'help'");
    Serial.print("> ");
}

void loop() {
    cliTick();
    wifiEnsure();
    app.loop();
    mqttLoop();

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
