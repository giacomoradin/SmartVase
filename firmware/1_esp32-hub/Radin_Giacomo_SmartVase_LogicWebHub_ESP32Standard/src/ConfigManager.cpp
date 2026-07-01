/*! @file ConfigManager.cpp
 *  @ingroup HubConfig
 *  @brief Implementation of ConfigManager: NVS I/O, CRC validation,
 *  bench fallback from secrets.h.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include "ConfigManager.h" // <-- THE CRUCIAL LINE THAT WAS MISSING!

#include "secrets.h"       // bench credentials (gitignored; see secrets.h.example)
#include "crc_utils.h"     // crc16_ibm (shared)
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h> // For memset and memcpy
#include <stdint.h> // For uint16_t etc.

// Tag for this module's log messages
static const char *TAG = "ConfigManager";

#define NVS_NAMESPACE "smartvase_cfg"   ///< NVS (Preferences) namespace for the Hub configuration.
#define NVS_CONFIG_KEY "device_config"  ///< NVS key of the `DeviceConfig` blob.

/*! @brief Magic number marking a valid `DeviceConfig` blob in NVS. */
#define CONFIG_MAGIC_NUMBER 0xCF6BEEF6

// CRC16 for NVS blob integrity: IBM/ARC (poly 0xA001), implementation shared
// in crc_utils. NOTE: this is DIFFERENT from the serial protocol's CRC (CCITT,
// see SerialManager/Mega) and that's intentional -- two independent uses.
namespace {
    /*! @brief Computes the CRC16-IBM of the entire `DeviceConfig` with the
     *  `crc16` field zeroed out, so that it is reproducible both when
     *  writing and when reading.
     *  @param[in] cfg Configuration to compute the checksum over (passed by
     *  copy internally, so the crc16 field can be zeroed without altering the original).
     *  @return CRC16-IBM computed over the struct's bytes.
     *  @note The `crc16` field sits in the middle of the struct: computing
     *  the CRC with the old value still inside it (as the previous version
     *  did) made verification on load impossible -- the NVS config always
     *  appeared corrupted and the credentials were wiped on every boot. */
    uint16_t config_crc(const DeviceConfig& cfg) {
        DeviceConfig tmp = cfg;
        tmp.crc16 = 0;
        return crc16_ibm(reinterpret_cast<const uint8_t*>(&tmp), sizeof(DeviceConfig));
    }
}


// --- ConfigManager Class Method Implementation ---

ConfigManager::ConfigManager() {
    // Initialize the in-memory configuration with empty/safe default values
    memset(&_config, 0, sizeof(DeviceConfig));
    _config.magic_number = CONFIG_MAGIC_NUMBER; // Set the magic number right away
    strcpy(_config.wifi_ssid, "");
    strcpy(_config.wifi_password, "");
    strcpy(_config.mqtt_broker, "");
    _config.mqtt_port = 1883; // Standard MQTT port
    strcpy(_config.mqtt_user, "");
    strcpy(_config.mqtt_password, "");
    strcpy(_config.webhook_url, "");
    // Add further default values here if needed
}

bool ConfigManager::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // If the NVS partition is corrupted or on an old version, erase and reinitialize it
        ESP_LOGW(TAG, "NVS partition damaged or outdated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init(); // Retry initialization
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "NVS initialized successfully.");
    return true;
}

bool ConfigManager::loadConfig() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle (%s)", esp_err_to_name(err));
        return false;
    }

    DeviceConfig loaded_config;
    // Zero-initialize loaded_config for safety before reading
    memset(&loaded_config, 0, sizeof(DeviceConfig));
    size_t required_size = sizeof(DeviceConfig);
    err = nvs_get_blob(nvs_handle, NVS_CONFIG_KEY, &loaded_config, &required_size);

    bool load_successful = false;
    if (err == ESP_OK) {
        // Verify Magic Number and Size BEFORE computing the CRC
        if (required_size == sizeof(DeviceConfig) && loaded_config.magic_number == CONFIG_MAGIC_NUMBER) {
            // CRC over a copy with the crc16 field zeroed out (see config_crc)
            uint16_t calculated_crc = config_crc(loaded_config);

            if (calculated_crc == loaded_config.crc16) {
                // CRC and Magic Number match, valid configuration!
                memcpy(&_config, &loaded_config, sizeof(DeviceConfig));
                load_successful = true;
                ESP_LOGI(TAG, "Configuration loaded successfully from NVS.");
            } else {
                ESP_LOGW(TAG, "NVS CRC mismatch! Calculated=0x%X, Stored=0x%X", calculated_crc, loaded_config.crc16);
            }
        } else {
             // Print specific values for debugging
             ESP_LOGW(TAG, "NVS magic/size mismatch! Read Size=%d, Expected Size=%d, Read Magic=0x%lX, Expected Magic=0x%lX",
                     required_size, sizeof(DeviceConfig), (unsigned long)loaded_config.magic_number, (unsigned long)CONFIG_MAGIC_NUMBER);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Configuration key '%s' not found in NVS.", NVS_CONFIG_KEY);
    } else {
        ESP_LOGE(TAG, "Error reading configuration blob from NVS (%s)", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);

    // NVS-first: if the config is valid AND contains Wi-Fi credentials, use it.
    // This way, provisioning via CLI (`set` + `save`) is honored after a
    // reboot (it used to be broken by the old bench bypass that returned early).
    bool provisioned = load_successful && _config.wifi_ssid[0] != '\0';
    if (provisioned) {
        ESP_LOGI(TAG, "Configuration loaded from NVS (provisioned).");
        return true;
    }

    // NVS empty / invalid / without Wi-Fi: start from clean defaults.
    memset(&_config, 0, sizeof(DeviceConfig));
    _config.magic_number = CONFIG_MAGIC_NUMBER;
    _config.mqtt_port    = 1883;

#ifdef SV_BENCH_MODE
    // BENCH fallback: credentials from secrets.h (gitignored) for immediate
    // testing without provisioning. Kept in RAM only (NOT saved): editing
    // secrets.h takes effect on every boot, and a `set`+`save` from the CLI
    // moves the config permanently to NVS (which then wins from that point on, see 'provisioned').
    setWifiCredentials(SV_WIFI_SSID, SV_WIFI_PASS);
    setMqttConfig(SV_MQTT_BROKER, SV_MQTT_PORT, SV_MQTT_USER, SV_MQTT_PASS);
    ESP_LOGW(TAG, "NVS vuota: uso credenziali di fallback da secrets.h (SV_BENCH_MODE).");
    return true;
#else
    // Production: no credentials -> provisioning via CLI or AP required. Initialize NVS.
    ESP_LOGW(TAG, "NVS vuota: nessuna credenziale, provisioning necessario.");
    return saveConfig();
#endif
}


bool ConfigManager::saveConfig() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for writing (%s)", esp_err_to_name(err));
        return false;
    }

     // Make sure the magic number is correct before saving (even though it should already be)
    _config.magic_number = CONFIG_MAGIC_NUMBER;

    // CRC over a copy with the crc16 field zeroed out (see config_crc)
    _config.crc16 = config_crc(_config);

    // Write the whole struct as a blob
    err = nvs_set_blob(nvs_handle, NVS_CONFIG_KEY, &_config, sizeof(DeviceConfig));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing configuration blob to NVS (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Commit the changes to NVS
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing configuration changes to NVS (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Configuration saved successfully to NVS. CRC=0x%X", _config.crc16);
    return true;
}

// --- Getter Methods ---

const char* ConfigManager::getWifiSsid() const {
    return _config.wifi_ssid;
}

const char* ConfigManager::getWifiPassword() const {
    return _config.wifi_password;
}

const char* ConfigManager::getMqttBroker() const {
    return _config.mqtt_broker;
}

uint16_t ConfigManager::getMqttPort() const {
    return _config.mqtt_port;
}

const char* ConfigManager::getMqttUser() const {
    return _config.mqtt_user;
}

const char* ConfigManager::getMqttPassword() const {
    return _config.mqtt_password;
}

const char* ConfigManager::getWebhookUrl() const {
    return _config.webhook_url;
}

// --- Setter Methods (to update the in-memory configuration before saving it) ---

void ConfigManager::setWifiCredentials(const char* ssid, const char* password) {
    if (ssid) {
        strncpy(_config.wifi_ssid, ssid, sizeof(_config.wifi_ssid) - 1);
        _config.wifi_ssid[sizeof(_config.wifi_ssid) - 1] = '\0';
    } else {
        _config.wifi_ssid[0] = '\0';
    }

    if (password) {
        strncpy(_config.wifi_password, password, sizeof(_config.wifi_password) - 1);
        _config.wifi_password[sizeof(_config.wifi_password) - 1] = '\0';
    } else {
         _config.wifi_password[0] = '\0';
    }
}

void ConfigManager::setMqttConfig(const char* broker, uint16_t port, const char* user, const char* password) {
    if (broker) {
        strncpy(_config.mqtt_broker, broker, sizeof(_config.mqtt_broker) - 1);
        _config.mqtt_broker[sizeof(_config.mqtt_broker) - 1] = '\0';
    } else {
         _config.mqtt_broker[0] = '\0';
    }

    _config.mqtt_port = port;

    if (user) {
        strncpy(_config.mqtt_user, user, sizeof(_config.mqtt_user) - 1);
        _config.mqtt_user[sizeof(_config.mqtt_user) - 1] = '\0';
    } else {
         _config.mqtt_user[0] = '\0';
    }

    if (password) {
        strncpy(_config.mqtt_password, password, sizeof(_config.mqtt_password) - 1);
        _config.mqtt_password[sizeof(_config.mqtt_password) - 1] = '\0';
    } else {
         _config.mqtt_password[0] = '\0';
    }
}

void ConfigManager::setWebhookUrl(const char* url) {
     if (url) {
        strncpy(_config.webhook_url, url, sizeof(_config.webhook_url) - 1);
        _config.webhook_url[sizeof(_config.webhook_url) - 1] = '\0';
    } else {
         _config.webhook_url[0] = '\0';
    }
}

