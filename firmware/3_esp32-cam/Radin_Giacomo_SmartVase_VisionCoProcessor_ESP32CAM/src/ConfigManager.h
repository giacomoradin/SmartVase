#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

#define CAM_FW_VERSION "2.2.0"
#define NTP_VALID_EPOCH 1700000000UL

extern char deviceId[16];

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
};

struct DeviceStats {
    uint32_t successful_frames;
    uint32_t failed_frames;
    uint32_t upload_errors;
    uint32_t firebase_errors;
    uint64_t total_capture_time_ms;
};

extern CamConfig cfg;
extern DeviceStats stats;
extern Preferences prefs;
extern Preferences statsPrefs;

void makeDeviceId();
void loadConfig();
void saveConfig();
void loadStats();
void saveStats();

#endif // CONFIG_MANAGER_H
