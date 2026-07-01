/*! @file WifiManager.cpp
 *  @ingroup HubNetworking
 *  @brief Implementation of WifiManager: STA connection, NTP sync,
 *  provisioning AP with Captive Portal.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include "WifiManager.h"
#include "ConfigManager.h" // Needed to access the credentials
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // For the provisioning server
#include "esp_log.h"
#include <esp_wifi.h> // To read the MAC address
#include <time.h>     // configTime/NTP: needed for TLS cert validation

// Tag for this module's log messages
static const char *TAG = "WifiManager";

/*! @brief Epoch below which the system time is considered unsynchronized (NTP not achieved). */
#define NTP_VALID_EPOCH 1700000000UL

// Global flag (internal to the .cpp module) indicating that provisioning has
// finished and a reboot is required. Using an anonymous namespace to limit its visibility.
namespace {
    volatile bool provisioningComplete = false; // volatile because it's modified inside a handler
}

/*! @brief Starts SNTP synchronization and waits (at most ~5 s, blocking)
 *  for the system clock to reach a plausible epoch.
 *  @details Best-effort NTP: without a real time, TLS certificate validation
 *  towards HiveMQ fails ("certificate not yet valid") and the handshake
 *  never starts. The wait here is only a first attempt at boot: if by 5 s
 *  the time is still not valid, MqttManager::reconnect() forces further
 *  periodic SNTP retries before connecting anyway.
 *  @note Blocking via `delay(100)` steps: acceptable only because it is
 *  called once at startup (after a successful connect()), never from the
 *  FreeRTOS tasks during normal operation. */
static void syncTimeNtp() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    unsigned long t0 = millis();
    while (time(nullptr) < NTP_VALID_EPOCH && millis() - t0 < 5000) {
        delay(100);
    }
    if (time(nullptr) >= NTP_VALID_EPOCH) {
        ESP_LOGI(TAG, "NTP sync OK (epoch=%lu)", (unsigned long)time(nullptr));
    } else {
        ESP_LOGW(TAG, "NTP non sincronizzato: la TLS verso HiveMQ potrebbe fallire");
    }
}

// --- WifiManager Class Method Implementation ---

// Constructor: initializes the private members and the provisioning web server
WifiManager::WifiManager(ConfigManager& configMgr)
    : _configMgr(configMgr),            // Initialize the reference to ConfigManager
      _isConnected(false),              // Initialize the connection state
      _provisioningServer(80)           // Initialize the server on port 80
{
    // Clear the temporary buffers at the start
    memset(_tempSsid, 0, sizeof(_tempSsid));
    memset(_tempPassword, 0, sizeof(_tempPassword));
}

// Attempts to connect to Wi-Fi with a timeout; the boot proceeds regardless.
//
// If credentials are missing from NVS, or if the STA connection does not
// succeed within WIFI_CONNECT_TIMEOUT_MS, starts the provisioning AP
// "SmartVase_Setup_XXXX" + Captive Portal (see startProvisioningAP()).
// NVS credentials are never cleared on failure: a new attempt can also be
// made from the serial CLI (`wifi connect`).
void WifiManager::connect() {
    const char* ssid = _configMgr.getWifiSsid();
    const char* password = _configMgr.getWifiPassword();

    // Radio always in STA: also needed to read the MAC (device_id, MQTT
    // client id) and allows a later retry via CLI `wifi connect`.
    WiFi.mode(WIFI_STA);

    if (ssid == nullptr || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "Nessuna credenziale Wi-Fi in NVS: avvio AP di provisioning.");
        startProvisioningAP();
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi network: %s", ssid);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime <= WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print("."); // Visual feedback on the console
    }
    Serial.println("");

    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "WiFi Connected! IP Address: %s", WiFi.localIP().toString().c_str());
        _isConnected = true;
        syncTimeNtp();
    } else {
        ESP_LOGW(TAG, "Wi-Fi non connesso entro %d ms: avvio AP di provisioning.", WIFI_CONNECT_TIMEOUT_MS);
        _isConnected = false;
        startProvisioningAP();
    }
}

// Returns true if connected to the main Wi-Fi network, false otherwise.
bool WifiManager::isConnected() const {
    // Check both our internal flag and the actual Wi-Fi state
    return _isConnected && (WiFi.status() == WL_CONNECTED);
}

// Starts Access Point (AP) mode for provisioning
void WifiManager::startProvisioningAP() {
    _isConnected = false;           // We are not connected to the main network
    provisioningComplete = false;   // Reset the completion flag
    memset(_tempSsid, 0, sizeof(_tempSsid)); // Clear the temporary buffers
    memset(_tempPassword, 0, sizeof(_tempPassword));

    // Generate a unique SSID for the AP based on the ESP32's MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac); // Get the MAC address
    char ap_ssid[32];
    // Build an SSID like "SmartVase_Setup_A1B2" using the last two MAC bytes
    snprintf(ap_ssid, sizeof(ap_ssid), "SmartVase_Setup_%02X%02X", mac[4], mac[5]);

    ESP_LOGI(TAG, "Starting Access Point: %s", ap_ssid);
    WiFi.disconnect(true); // Make sure we're disconnected from any previous STA network
    delay(100);            // Small pause
    WiFi.mode(WIFI_AP);    // Set Access Point mode

    // Configure the AP (no password, for simplicity)
    WiFi.softAP(ap_ssid);

    IPAddress apIP = WiFi.softAPIP(); // Get the AP's IP (usually 192.168.4.1)
    ESP_LOGI(TAG, "AP IP address: %s", apIP.toString().c_str());
    ESP_LOGI(TAG, "Connect to '%s' network and navigate to http://%s to configure WiFi.", ap_ssid, apIP.toString().c_str());

    // Start the DNS server for the Captive Portal, redirecting all requests to the AP
    _dnsServer.start(53, "*", apIP);

    // Configure the web server endpoints for the provisioning page
    setupProvisioningServer();
    _provisioningServer.begin(); // Start the class's member web server
}

// Handles the operations required while in AP mode (to be called from the loop/task).
void WifiManager::handleProvisioning() {
    // If we're not in AP mode, there's nothing to do here
    if (WiFi.getMode() != WIFI_AP) {
        return;
    }

    // Process the Captive Portal's DNS requests
    _dnsServer.processNextRequest();

    // Check whether the `provisioningComplete` flag has been set by the /save handler
    if (provisioningComplete) {
        // Use a helper function for the save-and-restart logic
        completeProvisioning();
        // Note: completeProvisioning() calls ESP.restart(), so the code below
        // will not run after the reboot.
    }
}

// Configures the endpoints (routes) of the provisioning web server
void WifiManager::setupProvisioningServer() {
    // Handler for the main page (GET /) - Shows the premium HTML form
    _provisioningServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = R"raw(
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SmartVase — Configurazione Wi-Fi</title>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600&display=swap" rel="stylesheet">
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Outfit', sans-serif; }
        body {
            background: linear-gradient(135deg, #0f2017 0%, #173321 50%, #20442b 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
            color: #e2f0e6;
        }
        .card {
            background: rgba(255, 255, 255, 0.06);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 24px;
            padding: 40px 30px;
            width: 100%;
            max-width: 440px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.4);
            animation: fadeIn 0.8s ease-out;
        }
        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }
        .header {
            text-align: center;
            margin-bottom: 35px;
        }
        .logo-container {
            display: inline-flex;
            align-items: center;
            justify-content: center;
            width: 64px;
            height: 64px;
            background: linear-gradient(135deg, #42bd6a 0%, #248a47 100%);
            border-radius: 18px;
            margin-bottom: 16px;
            box-shadow: 0 8px 16px rgba(36,138,71,0.3);
        }
        .logo-icon {
            font-size: 32px;
        }
        h2 {
            font-size: 26px;
            font-weight: 600;
            color: #ffffff;
            letter-spacing: -0.5px;
        }
        p.subtitle {
            font-size: 14px;
            color: #a4c4b0;
            margin-top: 6px;
        }
        .form-group {
            margin-bottom: 22px;
        }
        label {
            display: block;
            font-size: 13px;
            font-weight: 600;
            color: #a4c4b0;
            margin-bottom: 8px;
            text-transform: uppercase;
            letter-spacing: 0.8px;
        }
        input {
            width: 100%;
            background: rgba(0, 0, 0, 0.25);
            border: 1.5px solid rgba(255, 255, 255, 0.1);
            border-radius: 12px;
            padding: 14px 16px;
            color: #ffffff;
            font-size: 15px;
            outline: none;
            transition: all 0.3s ease;
        }
        input:focus {
            border-color: #42bd6a;
            background: rgba(0, 0, 0, 0.4);
            box-shadow: 0 0 12px rgba(66,189,106,0.15);
        }
        button {
            width: 100%;
            background: linear-gradient(135deg, #42bd6a 0%, #248a47 100%);
            border: none;
            border-radius: 12px;
            padding: 16px;
            color: #ffffff;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            box-shadow: 0 8px 16px rgba(36,138,71,0.3);
            transition: all 0.3s ease;
            margin-top: 10px;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 12px 20px rgba(36,138,71,0.45);
        }
        button:active {
            transform: translateY(0);
        }
        .loader {
            display: none;
            text-align: center;
            margin-top: 20px;
            animation: fadeIn 0.4s ease-out;
        }
        .spinner {
            display: inline-block;
            width: 36px;
            height: 36px;
            border: 3px solid rgba(255, 255, 255, 0.1);
            border-radius: 50%;
            border-top-color: #42bd6a;
            animation: spin 1s ease-in-out infinite;
        }
        @keyframes spin {
            to { transform: rotate(360deg); }
        }
        .loader p {
            font-size: 14px;
            color: #a4c4b0;
            margin-top: 12px;
        }
    </style>
    <script>
        function showLoader() {
            document.getElementById('setup-form').style.display = 'none';
            document.getElementById('form-loader').style.display = 'block';
        }
    </script>
</head>
<body>
    <div class="card">
        <div class="header">
            <div class="logo-container">
                <span class="logo-icon">🌿</span>
            </div>
            <h2>SmartVase</h2>
            <p class="subtitle">Configurazione Rete Wi-Fi</p>
        </div>
        <form id="setup-form" action="/save" method="post" onsubmit="showLoader()">
            <div class="form-group">
                <label for="ssid">Nome Rete (SSID)</label>
                <input type="text" id="ssid" name="ssid" placeholder="es. Wi-Fi Casa" required autocomplete="off">
            </div>
            <div class="form-group">
                <label for="pass">Password Rete</label>
                <input type="password" id="pass" name="pass" placeholder="Inserisci password" autocomplete="off">
            </div>
            <button type="submit">Connetti Dispositivo</button>
        </form>
        <div id="form-loader" class="loader">
            <div class="spinner"></div>
            <p>Salvataggio credenziali e riavvio in corso...</p>
        </div>
    </div>
</body>
</html>
        )raw";
        request->send(200, "text/html", html);
    });

    // Handler for the /save endpoint (POST) - Receives the credentials from the form
    _provisioningServer.on("/save", HTTP_POST, [this](AsyncWebServerRequest *request){
        bool success = false;

        if (request->hasParam("ssid", true)) {
            const AsyncWebParameter* p_ssid = request->getParam("ssid", true);
            strncpy(_tempSsid, p_ssid->value().c_str(), sizeof(_tempSsid) - 1);
            _tempSsid[sizeof(_tempSsid) - 1] = '\0';

            if (request->hasParam("pass", true)) {
                const AsyncWebParameter* p_pass = request->getParam("pass", true);
                strncpy(_tempPassword, p_pass->value().c_str(), sizeof(_tempPassword) - 1);
                _tempPassword[sizeof(_tempPassword) - 1] = '\0';
            } else {
                _tempPassword[0] = '\0';
            }

            ESP_LOGI(TAG, "Received WiFi credentials via AP: SSID='%s'", _tempSsid);
            success = true;
            provisioningComplete = true;
        } else {
            ESP_LOGE(TAG, "SSID parameter missing in POST request to /save.");
        }

        String htmlResponse = R"raw(
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SmartVase — Connessione</title>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600&display=swap" rel="stylesheet">
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Outfit', sans-serif; }
        body {
            background: linear-gradient(135deg, #0f2017 0%, #173321 50%, #20442b 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
            color: #e2f0e6;
            text-align: center;
        }
        .card {
            background: rgba(255, 255, 255, 0.06);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 24px;
            padding: 40px 30px;
            width: 100%;
            max-width: 440px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.4);
            animation: fadeIn 0.8s ease-out;
        }
        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(20px); }
            to { opacity: 1; transform: translateY(0); }
        }
        .icon {
            font-size: 48px;
            margin-bottom: 20px;
            display: inline-block;
            animation: bounce 2s infinite;
        }
        @keyframes bounce {
            0%, 100% { transform: translateY(0); }
            50% { transform: translateY(-10px); }
        }
        h2 {
            font-size: 22px;
            color: #ffffff;
            margin-bottom: 12px;
        }
        p {
            font-size: 15px;
            color: #a4c4b0;
            line-height: 1.6;
        }
    </style>
</head>
<body>
    <div class="card">
        <span class="icon">🚀</span>
        <h2>Credenziali Ricevute!</h2>
        <p>Il dispositivo si sta riavviando per connettersi alla tua rete Wi-Fi. Puoi chiudere questa pagina e ricollegarti alla tua rete domestica.</p>
    </div>
</body>
</html>
        )raw";
        request->send(200, "text/html", htmlResponse);
    });

    // Handler for the Captive Portal (redirects any other undefined request to the root)
    _provisioningServer.onNotFound([](AsyncWebServerRequest *request){
        ESP_LOGD(TAG, "Captive portal redirect: %s", request->url().c_str());
        request->redirect("http://192.168.4.1/");
    });
}

// Helper function called internally once provisioning is complete
void WifiManager::completeProvisioning() {
    ESP_LOGI(TAG, "Provisioning complete flag detected. Saving credentials and rebooting...");

    // Stop the DNS server and the AP web server
    _dnsServer.stop();
    _provisioningServer.end();

    // Disconnect and shut down the Access Point
    WiFi.softAPdisconnect(true);

    // Set Wi-Fi mode to OFF, for safety, before saving/restarting
    WiFi.mode(WIFI_OFF);
    delay(100); // Short pause

    // Save the newly received credentials via the ConfigManager
    _configMgr.setWifiCredentials(_tempSsid, _tempPassword);
    if (_configMgr.saveConfig()) {
        ESP_LOGI(TAG, "Credentials saved successfully to NVS. Restarting ESP...");
    } else {
        ESP_LOGE(TAG, "CRITICAL: Failed to save credentials to NVS! Restarting anyway...");
    }

    delay(1000);
    ESP.restart(); // Restart the ESP32
}

