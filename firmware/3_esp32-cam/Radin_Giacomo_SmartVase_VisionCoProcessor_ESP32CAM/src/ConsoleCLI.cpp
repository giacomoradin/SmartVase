#include "ConsoleCLI.h"
#include "ConfigManager.h"
#include "CameraDriver.h"
#include "CloudService.h"
#include <WiFi.h>
#include <time.h>

//buffer and index tracking serial command line input
static char cliBuf[192];
static size_t cliPos = 0;

//print list of available serial console commands
static void cliPrintHelp() {
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

//print current hardware network and time synchronization status
static void cliPrintStatus() {
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

//print active configuration settings stored in nvs memory
static void cliPrintShow() {
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

//print cumulative capture and error counters
static void cliPrintStats() {
    Serial.println("--- statistics ---");
    Serial.printf("successful_frames     = %lu\n", (unsigned long)stats.successful_frames);
    Serial.printf("failed_frames         = %lu\n", (unsigned long)stats.failed_frames);
    Serial.printf("upload_errors         = %lu\n", (unsigned long)stats.upload_errors);
    Serial.printf("firebase_errors       = %lu\n", (unsigned long)stats.firebase_errors);
    Serial.printf("total_capture_time_ms = %llu\n", stats.total_capture_time_ms);
}

//parse and apply configuration parameter updates from console
static bool cliHandleSet(char* args) {
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

//execute parsed command string entered by user
static void cliExecute(char* line) {
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

//read characters from serial buffer and execute completed command lines
void cliTick() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        //echo received character back to hardware terminal
        Serial.write(c);
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
