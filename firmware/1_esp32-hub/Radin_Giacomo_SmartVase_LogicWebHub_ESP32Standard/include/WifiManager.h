#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Libreria per il server web di provisioning
#include <DNSServer.h>          // Libreria per il Captive Portal DNS
#include "ConfigManager.h"     // Includi ConfigManager per accedere ai dati

// Definisci qui le costanti usate dalla classe
#define SSID_MAX_LENGTH         32  // Lunghezza massima SSID (standard WiFi)
#define PASSWORD_MAX_LENGTH     64  // Lunghezza massima Password WiFi (standard WiFi)
#define WIFI_CONNECT_TIMEOUT_MS 15000 // Timeout connessione WiFi (15 secondi)

class WifiManager {
public:
    // Costruttore: riceve il ConfigManager per accedere alle credenziali
    WifiManager(ConfigManager& configMgr);

    // Tenta la connessione al WiFi usando le credenziali salvate.
    // Se non presenti o errate, avvia la modalità Access Point (AP) per il provisioning.
    void connect();

    // Restituisce true se connesso alla rete WiFi principale, false altrimenti.
    bool isConnected() const;

    // Gestisce le operazioni necessarie durante la modalità AP (da chiamare nel loop/task).
    // Principalmente controlla se il provisioning è stato completato per riavviare.
    void handleProvisioning();

private:
    // Riferimento (non copia) all'oggetto ConfigManager creato in main.cpp
    ConfigManager& _configMgr;

    // Flag che indica se siamo connessi alla rete WiFi principale
    bool _isConnected;

    // Server web usato *solo* durante la modalità Access Point per il provisioning
    AsyncWebServer _provisioningServer;

    // Server DNS per il Captive Portal
    DNSServer _dnsServer;

    // Buffer temporanei per memorizzare le credenziali ricevute via AP
    // prima di salvarle definitivamente su NVS e riavviare.
    char _tempSsid[SSID_MAX_LENGTH];
    char _tempPassword[PASSWORD_MAX_LENGTH];

    // Avvia la modalità Access Point (AP)
    void startProvisioningAP();

    // Configura gli endpoint (route) del server web di provisioning
    void setupProvisioningServer();

    // Funzione helper chiamata internamente quando il provisioning è completato
    // per salvare le credenziali e riavviare l'ESP.
    void completeProvisioning();
};

#endif // WIFIMANAGER_H

