#include "ConfigManager.h"
#include "secrets.h"

char deviceId[16] = {0};
CamConfig cfg;
DeviceStats stats;
Preferences prefs;
Preferences statsPrefs;

void makeDeviceId() {
    snprintf(deviceId, sizeof(deviceId), "CAM_123456");
}

void loadConfig() {
    prefs.begin("cam", true);
    cfg.wifi_ssid           = prefs.getString("wifi_ssid",           "");
    cfg.wifi_pass           = prefs.getString("wifi_pass",           "");
    cfg.upload_url          = prefs.getString("upload_url",          "");
    cfg.interval_s          = prefs.getUInt  ("interval_s",          300);
    cfg.firebase_api_key    = prefs.getString("firebase_api_key",    "");
    cfg.firebase_project_id = prefs.getString("firebase_project_id", "");
    cfg.firebase_email      = prefs.getString("firebase_email",      "");
    cfg.firebase_password   = prefs.getString("firebase_password",   "");
    cfg.roi_center_x        = prefs.getUInt  ("roi_cx",              400);
    cfg.roi_center_y        = prefs.getUInt  ("roi_cy",              300);
    cfg.roi_radius          = prefs.getUInt  ("roi_r",               280);
    prefs.end();

#ifdef SMARTVASE_CAM_SECRETS_H
    cfg.wifi_ssid           = SECRET_WIFI_SSID;
    cfg.wifi_pass           = SECRET_WIFI_PASS;
    cfg.firebase_api_key    = SECRET_FIREBASE_API_KEY;
    cfg.firebase_project_id = SECRET_FIREBASE_PROJECT_ID;
    cfg.firebase_email      = SECRET_FIREBASE_EMAIL;
    cfg.firebase_password   = SECRET_FIREBASE_PASSWORD;
#else
    cfg.wifi_ssid   = "XXL";
    cfg.wifi_pass   = "pomodoro";
#endif
}

void saveConfig() {
    prefs.begin("cam", false);
    prefs.putString("wifi_ssid",           cfg.wifi_ssid);
    prefs.putString("wifi_pass",           cfg.wifi_pass);
    prefs.putString("firebase_api_key",    cfg.firebase_api_key);
    prefs.putString("firebase_project_id", cfg.firebase_project_id);
    prefs.putString("firebase_email",      cfg.firebase_email);
    prefs.putString("firebase_password",   cfg.firebase_password);
    prefs.putString("upload_url",          cfg.upload_url);
    prefs.putUInt  ("interval_s",          cfg.interval_s);
    prefs.putUInt  ("roi_cx",              cfg.roi_center_x);
    prefs.putUInt  ("roi_cy",              cfg.roi_center_y);
    prefs.putUInt  ("roi_r",               cfg.roi_radius);
    prefs.end();
}

void loadStats() {
    statsPrefs.begin("cam_stats", true);
    stats.successful_frames     = statsPrefs.getUInt   ("succ_frames",  0);
    stats.failed_frames         = statsPrefs.getUInt   ("fail_frames",  0);
    stats.upload_errors         = statsPrefs.getUInt   ("upload_err",   0);
    stats.firebase_errors       = statsPrefs.getUInt   ("fb_err",       0);
    stats.total_capture_time_ms = statsPrefs.getULong64("total_cap_ms", 0);
    statsPrefs.end();
}

void saveStats() {
    statsPrefs.begin("cam_stats", false);
    statsPrefs.putUInt   ("succ_frames",  stats.successful_frames);
    statsPrefs.putUInt   ("fail_frames",  stats.failed_frames);
    statsPrefs.putUInt   ("upload_err",   stats.upload_errors);
    statsPrefs.putUInt   ("fb_err",       stats.firebase_errors);
    statsPrefs.putULong64("total_cap_ms", stats.total_capture_time_ms);
    statsPrefs.end();
}
