#include "WifiManager.h"
#include "ConfigManager.h" // Necessario per accedere alle credenziali
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Per il server di provisioning
#include "esp_log.h"
#include <esp_wifi.h> // Per leggere l'indirizzo MAC

// Tag per i log specifici di questo modulo
static const char *TAG = "WifiManager";

// Flag globale (interno al modulo .cpp) per indicare quando il provisioning è terminato
// e bisogna riavviare. Usiamo un namespace anonimo per limitarne la visibilità.
namespace {
    volatile bool provisioningComplete = false; // volatile perché modificato in un handler
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
        ESP_LOGW(TAG, "Nessuna credenziale Wi-Fi in NVS: si continua offline.");
        Serial.println(F("[WiFi] Non configurato. Dalla CLI: set wifi_ssid <ssid>, set wifi_pass <pwd>, save, reboot"));
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
    } else {
        ESP_LOGW(TAG, "Wi-Fi non connesso entro %d ms: si continua offline "
                      "(retry automatico in background).", WIFI_CONNECT_TIMEOUT_MS);
        Serial.println(F("[WiFi] Offline. Verifica credenziali con 'show', riprova con 'wifi connect'."));
        _isConnected = false;
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

    // Controlla se il flag `provisioningComplete` è stato impostato dall'handler /save
    if (provisioningComplete) {
        // Usa una funzione helper per la logica di salvataggio e riavvio
        completeProvisioning();
        // Nota: completeProvisioning() chiama ESP.restart(), quindi il codice qui sotto
        // non verrà eseguito dopo il riavvio.
    }

    // ESPAsyncWebServer gestisce le richieste client in background.
    // Non è necessario chiamare server.handleClient() come con il WebServer standard.
    // Potremmo aggiungere qui una logica di timeout se l'utente non configura entro X minuti.
}

// Configura gli endpoint (route) del server web di provisioning
void WifiManager::setupProvisioningServer() {
    // Handler per la pagina principale (GET /) - Mostra il form HTML
    _provisioningServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        // HTML del form (incorporato come stringa raw per leggibilità)
        String html = R"(
            <!DOCTYPE html><html><head><title>SmartVase WiFi Setup</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>
                body { font-family: sans-serif; padding: 20px; background-color: #f4f4f4; color: #333; }
                .container { background-color: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 400px; margin: 40px auto; }
                input[type=text], input[type=password] { width: 100%; padding: 12px 20px; margin: 8px 0; display: inline-block; border: 1px solid #ccc; box-sizing: border-box; border-radius: 4px; }
                button { background-color: #4CAF50; color: white; padding: 14px 20px; margin: 20px 0 10px 0; border: none; cursor: pointer; width: 100%; border-radius: 4px; font-size: 16px; }
                button:hover { opacity: 0.9; }
                h2 { text-align: center; color: #4CAF50; margin-bottom: 30px; }
                label { font-weight: bold; }
            </style></head><body>
            <div class="container">
            <h2>SmartVase WiFi Setup</h2>
            <form action="/save" method="post">
                <label for="ssid">WiFi Network (SSID)</label>
                <input type="text" placeholder="Enter Network Name" name="ssid" required>
                <label for="pass">Password</label>
                <input type="password" placeholder="Enter Network Password" name="pass">
                <button type="submit">Save & Connect</button>
            </form></div></body></html>
        )";
        // Invia la pagina HTML al client
        request->send(200, "text/html", html);
    });

    // Handler per l'endpoint /save (POST) - Riceve le credenziali dal form
    // Usiamo una lambda con cattura [this] per accedere ai membri della classe WifiManager
    _provisioningServer.on("/save", HTTP_POST, [this](AsyncWebServerRequest *request){
        String message = "Credentials Received. Restarting device..."; // Messaggio di default
        bool success = false;

        // Controlla se il parametro 'ssid' è stato inviato nel POST
        if (request->hasParam("ssid", true)) { // true = cerca nei parametri POST
            // Ottieni il puntatore al parametro (ora correttamente const)
            const AsyncWebParameter* p_ssid = request->getParam("ssid", true);
            // Copia il valore nel buffer temporaneo, troncando se necessario
            strncpy(_tempSsid, p_ssid->value().c_str(), sizeof(_tempSsid) - 1);
            _tempSsid[sizeof(_tempSsid) - 1] = '\0'; // Assicura sempre il terminatore nullo

            // Controlla se è stata inviata anche la password (potrebbe essere vuota per reti aperte)
            if (request->hasParam("pass", true)) {
                const AsyncWebParameter* p_pass = request->getParam("pass", true);
                strncpy(_tempPassword, p_pass->value().c_str(), sizeof(_tempPassword) - 1);
                 _tempPassword[sizeof(_tempPassword) - 1] = '\0';
            } else {
                 _tempPassword[0] = '\0'; // Imposta password vuota se non fornita
            }

            ESP_LOGI(TAG, "Received WiFi credentials via AP: SSID='%s'", _tempSsid);
            // Non loggare la password per sicurezza: ESP_LOGI(TAG, "Password: '%s'", _tempPassword);
            success = true;
            provisioningComplete = true; // Imposta il flag per indicare che il provisioning è finito

        } else {
            // Errore: il parametro SSID mancava
            message = "Error: SSID parameter missing!";
            ESP_LOGE(TAG, "SSID parameter missing in POST request to /save.");
        }

        // Invia una pagina HTML di risposta al browser
        String htmlResponse = R"(
            <!DOCTYPE html><html><head><title>SmartVase Setup</title>
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>body { font-family: sans-serif; padding: 20px; text-align: center; background-color: #f4f4f4; }</style>
            </head><body><h2>)" + message + R"(</h2><p>The device will now restart and attempt to connect to your network.</p></body></html>)";
        request->send(200, "text/html", htmlResponse);

        // Il salvataggio e riavvio effettivo avverrà in handleProvisioning()
        // quando rileverà il flag provisioningComplete = true.
        // Questo dà tempo alla risposta HTTP di essere inviata correttamente.
    });

    // Handler per pagine non trovate (404)
     _provisioningServer.onNotFound([](AsyncWebServerRequest *request){
        ESP_LOGW(TAG, "Not found: %s", request->url().c_str());
        request->send(404, "text/plain", "Page Not Found");
    });
}

// Funzione helper chiamata internamente quando il provisioning è completato
void WifiManager::completeProvisioning() {
    ESP_LOGI(TAG, "Provisioning complete flag detected. Saving credentials and rebooting...");

    // Ferma il server web AP
    _provisioningServer.end();

    // Disconnette e spegne l'Access Point
    // Il parametro 'true' cancella anche la configurazione dell'AP dalla memoria
    WiFi.softAPdisconnect(true);

    // Imposta la modalità WiFi su OFF per sicurezza prima di salvare/riavviare
    WiFi.mode(WIFI_OFF);
    delay(100); // Piccola pausa

    // Salva le nuove credenziali ricevute (ora nei membri _tempSsid/_tempPassword)
    // usando il ConfigManager
    _configMgr.setWifiCredentials(_tempSsid, _tempPassword);
    if (_configMgr.saveConfig()) {
        ESP_LOGI(TAG, "Credentials saved successfully to NVS. Restarting ESP...");
    } else {
        // Errore grave: non siamo riusciti a salvare le credenziali!
        ESP_LOGE(TAG, "CRITICAL: Failed to save credentials to NVS! Restarting anyway...");
        // Loggare questo errore è fondamentale. Potrebbe indicare un problema con la NVS.
    }

    delay(1000); // Dai tempo ai log di essere eventualmente inviati (se ci fosse già MQTT)
    ESP.restart(); // Riavvia l'ESP32
}

