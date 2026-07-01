/*! @file ConfigManager.h
 *  @ingroup HubConfig
 *  @brief Management of the Hub's persistent configuration (NVS/Preferences).
 *  @details Defines the `DeviceConfig` struct (Wi-Fi credentials, MQTT broker,
 *  webhook) stored as a blob in NVS with a magic number + CRC16-IBM to detect
 *  corruption, and the `ConfigManager` class that loads/saves it and exposes
 *  getters/setters for it. It is the source of truth used by `WifiManager`,
 *  `MqttManager`, `MainLogic` and `HubCli`.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubConfig Configuration (NVS)
 *  @brief Persistence of the device configuration (Wi-Fi, MQTT, webhook) on
 *  NVS, with validation via magic number and CRC16.
 */

#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <Arduino.h> // For standard types such as uint16_t, size_t
#include <stdint.h>  // For uint32_t etc.

/*! @addtogroup HubConfig
 *  @{
 */

/*! @name Maximum lengths of the configuration fields (including the NUL terminator)
 *  @{
 */
#define WIFI_SSID_MAX_LENGTH     32  /**< Maximum length of the Wi-Fi SSID. */
#define WIFI_PASSWORD_MAX_LENGTH 64  /**< Maximum length of the Wi-Fi password. */
#define MQTT_BROKER_MAX_LENGTH   64  /**< Maximum length of the MQTT broker hostname/URL. */
#define MQTT_USER_MAX_LENGTH     32  /**< Maximum length of the MQTT username. */
#define MQTT_PASSWORD_MAX_LENGTH 32  /**< Maximum length of the MQTT password. */
#define WEBHOOK_URL_MAX_LENGTH   128 /**< Maximum length of the optional webhook URL. */
/*! @} */

/*!
 * @def HUB_DEVICE_ID
 * @brief Static Hub device ID, used in all MQTT topics and in the protobuf
 * messages towards the Mega.
 * @note Team decision (alignment with Firebase): it must NOT be derived from
 * the MAC address, unlike the MQTT client ID (see `MqttManager::init`).
 */
#define HUB_DEVICE_ID "HUB_123456"

/*!
 * @struct DeviceConfig
 * @brief Binary layout of ALL the configuration data persisted as a single
 * blob in NVS (namespace `smartvase_cfg`, key `device_config`).
 * @note The struct is written/read in full with `nvs_set_blob`/`nvs_get_blob`:
 * any change to the fields invalidates the already-saved blobs (the magic+CRC
 * check wins anyway, restarting from clean defaults).
 */
struct DeviceConfig {
    uint32_t magic_number;            /**< Magic number to recognize a valid blob (see CONFIG_MAGIC_NUMBER in ConfigManager.cpp). */
    uint16_t crc16;                   /**< CRC16-IBM computed over the whole struct with this field zeroed (see config_crc()). */

    // WiFi credentials
    char wifi_ssid[WIFI_SSID_MAX_LENGTH];         /**< SSID of the STA Wi-Fi network, NUL-terminated string. */
    char wifi_password[WIFI_PASSWORD_MAX_LENGTH]; /**< Password of the STA Wi-Fi network, NUL-terminated string. */

    // MQTT configuration
    char mqtt_broker[MQTT_BROKER_MAX_LENGTH];     /**< Hostname or IP of the MQTT broker. */
    uint16_t mqtt_port;                           /**< Broker port: 8883/8884 = TLS, 1883 = plaintext (see MqttManager::init). */
    char mqtt_user[MQTT_USER_MAX_LENGTH];         /**< Username for MQTT authentication. */
    char mqtt_password[MQTT_PASSWORD_MAX_LENGTH]; /**< Password for MQTT authentication. */

    // Other configuration
    char webhook_url[WEBHOOK_URL_MAX_LENGTH];     /**< Optional webhook URL (not yet used by the rest of the firmware). */
};

/*!
 * @class ConfigManager
 * @brief Loads and saves `DeviceConfig` to NVS, exposes the current values via
 * getters/setters to the other classes (WifiManager, MqttManager, HubCli).
 * @note Not thread-safe: it is used only by the main Arduino task
 * (setup()/loop(), including the CLI) and never by TaskMqttLink/TaskMainLogic
 * directly (those tasks only read references/copies passed to the constructor).
 */
class ConfigManager {
public:
    /*! @brief Constructor: initializes `_config` with safe default values
     *  (empty strings, MQTT port 1883) until loadConfig() is called. */
    ConfigManager();

    /*! @brief Initializes the underlying NVS subsystem (`nvs_flash_init`).
     *  @details If the NVS partition turns out to be corrupted or from an old
     *  version, it is automatically erased and reinitialized.
     *  @return true if NVS is ready for use, false on an unrecoverable error.
     *  @note To be called once in setup(), before loadConfig(). */
    bool init();

    /*! @brief Loads the configuration from NVS into memory (`_config`).
     *  @details Verifies the magic number, size and CRC16 of the read blob. If
     *  the blob is valid and contains a non-empty Wi-Fi SSID ("provisioned"),
     *  it is used as-is (NVS-first). Otherwise it restarts from clean defaults
     *  and, if compiled with `SV_BENCH_MODE`, populates the credentials from
     *  `secrets.h` IN RAM ONLY (not saved to NVS) to allow bench testing
     *  without manual provisioning.
     *  @return true if the final configuration (NVS or bench fallback) is usable,
     *  false only on an NVS access error.
     *  @note In the absence of `SV_BENCH_MODE` and of valid credentials, it
     *  still writes the defaults to NVS (saveConfig()) so that the subsequent
     *  provisioning finds a blob with consistent magic/CRC. */
    bool loadConfig();

    /*! @brief Saves the current in-memory configuration (`_config`) to NVS.
     *  @details Recomputes the magic number and CRC16 before writing the blob.
     *  @return true if the NVS write and commit succeeded.
     *  @note Blocking operation (flash access); it must be called only from
     *  non-time-critical contexts (CLI `save`, end of AP provisioning). */
    bool saveConfig();

    // --- Getter methods to access the configuration values ---
    /*! @brief @return Current Wi-Fi SSID (empty string if not configured). */
    const char* getWifiSsid() const;
    /*! @brief @return Current Wi-Fi password. */
    const char* getWifiPassword() const;
    /*! @brief @return Current MQTT broker hostname/IP. */
    const char* getMqttBroker() const;
    /*! @brief @return Current MQTT broker port. */
    uint16_t    getMqttPort() const;
    /*! @brief @return Current MQTT username. */
    const char* getMqttUser() const;
    /*! @brief @return Current MQTT password. */
    const char* getMqttPassword() const;
    /*! @brief @return Current webhook URL (not yet used). */
    const char* getWebhookUrl() const;

    // --- Setter methods to modify the in-memory configuration ---
    /*! @brief Sets the Wi-Fi credentials in memory.
     *  @param[in] ssid SSID to set; if nullptr the string is emptied.
     *  @param[in] password Password to set; if nullptr the string is emptied.
     *  @note Does not persist to NVS: call saveConfig() to make the change
     *  survive a reboot. The values are truncated to the maximum length of the
     *  field (WIFI_SSID_MAX_LENGTH/WIFI_PASSWORD_MAX_LENGTH - 1). */
    void setWifiCredentials(const char* ssid, const char* password);
    /*! @brief Sets the MQTT configuration in memory in one shot (broker, port, user, password).
     *  @param[in] broker Broker hostname/IP; nullptr empties the field.
     *  @param[in] port Broker port (8883/8884 = TLS, 1883 = plaintext).
     *  @param[in] user MQTT username; nullptr empties the field.
     *  @param[in] password MQTT password; nullptr empties the field.
     *  @note Does not persist to NVS: call saveConfig() to make the change
     *  survive a reboot. */
    void setMqttConfig(const char* broker, uint16_t port, const char* user, const char* password);
    /*! @brief Sets the webhook URL in memory.
     *  @param[in] url URL to set; if nullptr the string is emptied.
     *  @note Does not persist to NVS: call saveConfig() to make the change
     *  survive a reboot. */
    void setWebhookUrl(const char* url);

private:
    DeviceConfig _config; /**< Current configuration in RAM, synced to NVS by loadConfig()/saveConfig(). */
};

/*! @} */ // end of HubConfig group

#endif // CONFIGMANAGER_H
