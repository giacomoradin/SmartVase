#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

//firmware version string and ntp validity timestamp threshold
#define CAM_FW_VERSION "2.2.0"
#define NTP_VALID_EPOCH 1700000000UL

//unique device identifier string
extern char deviceId[16];

//structure holding all camera configuration settings
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

//structure holding cumulative operating statistics
struct DeviceStats {
    uint32_t successful_frames;
    uint32_t failed_frames;
    uint32_t upload_errors;
    uint32_t firebase_errors;
    uint64_t total_capture_time_ms;
};

//global instances for configuration, statistics, and nvs storage
extern CamConfig cfg;
extern DeviceStats stats;
extern Preferences prefs;
extern Preferences statsPrefs;

//generate device identifier string
void makeDeviceId();
//load saved configuration from nvs flash memory
void loadConfig();
//save current configuration to nvs flash memory
void saveConfig();
//load cumulative statistics from nvs flash memory
void loadStats();
//save cumulative statistics to nvs flash memory
void saveStats();

#endif // CONFIG_MANAGER_H
