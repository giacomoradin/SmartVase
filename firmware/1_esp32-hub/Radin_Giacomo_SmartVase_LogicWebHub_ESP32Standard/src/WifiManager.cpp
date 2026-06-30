#include "WifiManager.h"
#include "ConfigManager.h" // Necessario per accedere alle credenziali
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Per il server di provisioning
#include "esp_log.h"
#include <esp_wifi.h> // Per leggere l'indirizzo MAC
#include <time.h>     // configTime/NTP: serve alla validazione del cert TLS

// Tag per i log specifici di questo modulo
static const char *TAG = "WifiManager";

// Sotto questo epoch l'ora di sistema NON e' sincronizzata (NTP non riuscito).
#define NTP_VALID_EPOCH 1700000000UL

// Flag globale (interno al modulo .cpp) per indicare quando il provisioning è terminato
// e bisogna riavviare. Usiamo un namespace anonimo per limitarne la visibilità.
namespace {
    volatile bool provisioningComplete = false; // volatile perché modificato in un handler
}

// NTP best-effort: senza ora reale la verifica di validita' del certificato
// TLS verso HiveMQ fallisce ("certificate not yet valid") e l'handshake non
// parte. Attesa breve e non bloccante oltre ~5 s (al boot e' accettabile).
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

// --- Implementazione Metodi Classe WifiManager ---

// Costruttore: inizializza i membri privati e il server web di provisioning
WifiManager::WifiManager(ConfigManager& configMgr)
    : _configMgr(configMgr),            // Inizializza il riferimento a ConfigManager
      _isConnected(false),              // Inizializza lo stato della connessione
      _provisioningServer(80)           // Inizializza il server sulla porta 80
{
    // Pulisce i buffer temporanei all'inizio
    memset(_tempSsid, 0, sizeof(_tempSsid));
    memset(_tempPassword, 0, sizeof(_tempPassword));
}

// Tenta la connessione al WiFi con timeout; in ogni caso il boot prosegue.
//
// Comportamento rivisto per il bring-up: il fallimento della connessione NON
// cancella piu' le credenziali da NVS e NON avvia l'AP di provisioning (la
// configurazione si fa dalla CLI seriale). L'autoreconnect dell'SDK continua
// a riprovare in background, quindi l'Hub aggancia la rete appena disponibile.
void WifiManager::connect() {
    const char* ssid = _configMgr.getWifiSsid();
    const char* password = _configMgr.getWifiPassword();

    // Radio sempre in STA: serve anche per leggere il MAC (device_id, client
    // id MQTT) e permette un retry successivo via CLI `wifi connect`.
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
        Serial.print("."); // Feedback visivo sulla console
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

// Restituisce true se connesso alla rete WiFi principale, false altrimenti.
bool WifiManager::isConnected() const {
    // Controlla sia il nostro flag interno che lo stato reale del WiFi
    return _isConnected && (WiFi.status() == WL_CONNECTED);
}

// Avvia la modalità Access Point (AP) per il provisioning
void WifiManager::startProvisioningAP() {
    _isConnected = false;           // Non siamo connessi alla rete principale
    provisioningComplete = false;   // Resetta il flag di completamento
    memset(_tempSsid, 0, sizeof(_tempSsid)); // Pulisci i buffer temporanei
    memset(_tempPassword, 0, sizeof(_tempPassword));

    // Genera un SSID univoco per l'AP basato sul MAC address dell'ESP32
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac); // Ottieni il MAC address
    char ap_ssid[32];
    // Crea SSID tipo "SmartVase_Setup_A1B2" usando le ultime due cifre del MAC
    snprintf(ap_ssid, sizeof(ap_ssid), "SmartVase_Setup_%02X%02X", mac[4], mac[5]);

    ESP_LOGI(TAG, "Starting Access Point: %s", ap_ssid);
    WiFi.disconnect(true); // Assicurati di essere disconnesso da qualsiasi rete STA precedente
    delay(100);            // Pausa
    WiFi.mode(WIFI_AP);    // Imposta la modalità Access Point

    // Configura l'AP (SSID senza password per semplicità)
    WiFi.softAP(ap_ssid);

    IPAddress apIP = WiFi.softAPIP(); // Ottieni l'IP dell'AP (di solito 192.168.4.1)
    ESP_LOGI(TAG, "AP IP address: %s", apIP.toString().c_str());
    ESP_LOGI(TAG, "Connect to '%s' network and navigate to http://%s to configure WiFi.", ap_ssid, apIP.toString().c_str());

    // Avvia il server DNS per il Captive Portal reindirizzando tutte le richieste all'AP
    _dnsServer.start(53, "*", apIP);

    // Configura gli endpoint del web server per la pagina di provisioning
    setupProvisioningServer();
    _provisioningServer.begin(); // Avvia il server web membro della classe
}

// Gestisce le operazioni necessarie durante la modalità AP (da chiamare nel loop/task).
void WifiManager::handleProvisioning() {
    // Se non siamo in modalità AP, non c'è nulla da fare qui
    if (WiFi.getMode() != WIFI_AP) {
        return;
    }

    // Processa le richieste DNS del Captive Portal
    _dnsServer.processNextRequest();

    // Controlla se il flag `provisioningComplete` è stato impostato dall'handler /save
    if (provisioningComplete) {
        // Usa una funzione helper per la logica di salvataggio e riavvio
        completeProvisioning();
        // Nota: completeProvisioning() chiama ESP.restart(), quindi il codice qui sotto
        // non verrà eseguito dopo il riavvio.
    }
}

// Configura gli endpoint (route) del server web di provisioning
void WifiManager::setupProvisioningServer() {
    // Handler per la pagina principale (GET /) - Mostra il form HTML Premium
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

    // Handler per l'endpoint /save (POST) - Riceve le credenziali dal form
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

    // Handler per Captive Portal (reindirizza qualsiasi altra richiesta non definita alla radice)
    _provisioningServer.onNotFound([](AsyncWebServerRequest *request){
        ESP_LOGD(TAG, "Captive portal redirect: %s", request->url().c_str());
        request->redirect("http://192.168.4.1/");
    });
}

// Funzione helper chiamata internamente quando il provisioning è completato
void WifiManager::completeProvisioning() {
    ESP_LOGI(TAG, "Provisioning complete flag detected. Saving credentials and rebooting...");

    // Ferma il server DNS e il server web AP
    _dnsServer.stop();
    _provisioningServer.end();

    // Disconnette e spegne l'Access Point
    WiFi.softAPdisconnect(true);

    // Imposta la modalità WiFi su OFF per sicurezza prima di salvare/riavviare
    WiFi.mode(WIFI_OFF);
    delay(100); // Piccola pausa

    // Salva le nuove credenziali ricevute usando il ConfigManager
    _configMgr.setWifiCredentials(_tempSsid, _tempPassword);
    if (_configMgr.saveConfig()) {
        ESP_LOGI(TAG, "Credentials saved successfully to NVS. Restarting ESP...");
    } else {
        ESP_LOGE(TAG, "CRITICAL: Failed to save credentials to NVS! Restarting anyway...");
    }

    delay(1000); 
    ESP.restart(); // Riavvia l'ESP32
}

