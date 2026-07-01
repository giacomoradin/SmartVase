/*! @file WifiManager.h
 *  @ingroup HubNetworking
 *  @brief Wi-Fi STA connection management and provisioning via Access
 *  Point + Captive Portal.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubNetworking Networking (Wi-Fi and MQTT)
 *  @brief Connectivity to the local network (Wi-Fi, provisioning) and to the
 *  cloud (MQTT/TLS over HiveMQ).
 */

#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Library for the provisioning web server
#include <DNSServer.h>          // Library for the Captive Portal DNS
#include "ConfigManager.h"     // Include ConfigManager to access the data

/*! @addtogroup HubNetworking
 *  @{
 */

#define SSID_MAX_LENGTH         32  /**< Maximum SSID length (Wi-Fi standard) */
#define PASSWORD_MAX_LENGTH     64  /**< Maximum Wi-Fi password length (Wi-Fi standard) */
#define WIFI_CONNECT_TIMEOUT_MS 15000 /**< Wi-Fi connection timeout (15 seconds) */

/**
 * @class WifiManager
 * @brief Manager for Wi-Fi connectivity and the Captive Portal used to provision the ESP32 Hub.
 *
 * Attempts to connect in Station (STA) mode using the credentials stored in NVS.
 * If the connection fails or credentials are missing, it automatically starts a protected
 * Access Point (AP) and a DNS Server that redirects all HTTP requests (Captive Portal) to a
 * premium glassmorphism setup web page (to enter SSID and Password).
 *
 * @note This behavior (automatic AP startup on timeout/missing credentials)
 * has been active since v1.3; previously (v1.2) a connection failure did NOT
 * start the AP and provisioning was only done via the serial CLI (see the
 * historical comment on connect() in WifiManager.cpp).
 */
class WifiManager {
public:
    /**
     * @brief Constructor of the WifiManager class.
     * @param configMgr Reference to the ConfigManager instance used to read/write the configuration.
     */
    WifiManager(ConfigManager& configMgr);

    /**
     * @brief Attempts to connect to Wi-Fi.
     *
     * If it fails to connect within WIFI_CONNECT_TIMEOUT_MS or credentials are missing, starts the provisioning AP.
     */
    void connect();

    /**
     * @brief Returns whether the device is connected to the Wi-Fi network (in STA mode).
     * @return true if connected with a valid IP, false otherwise.
     */
    bool isConnected() const;

    /**
     * @brief Handles DNS server requests during the Captive Portal.
     *
     * To be called periodically in the main loop or in a dedicated task while the provisioning AP is active.
     */
    void handleProvisioning();

private:
    ConfigManager& _configMgr;                /**< Reference to the NVS configuration manager */
    bool _isConnected;                        /**< Active STA connection flag */
    AsyncWebServer _provisioningServer;       /**< Asynchronous web server for the configuration HTML interface */
    DNSServer _dnsServer;                     /**< DNS server used to intercept traffic and force the redirect */

    char _tempSsid[SSID_MAX_LENGTH];          /**< Temporary buffer for SSID */
    char _tempPassword[PASSWORD_MAX_LENGTH];  /**< Temporary buffer for password */

    /**
     * @brief Starts the local "SmartVase_Setup_XXXX" Access Point.
     */
    void startProvisioningAP();

    /**
     * @brief Registers the HTTP routes of the provisioning server (main HTML page, credential saving).
     */
    void setupProvisioningServer();

    /**
     * @brief Saves the new credentials to NVS via the ConfigManager and restarts the ESP32.
     */
    void completeProvisioning();
};

/*! @} */ // end of HubNetworking group

#endif // WIFIMANAGER_H

