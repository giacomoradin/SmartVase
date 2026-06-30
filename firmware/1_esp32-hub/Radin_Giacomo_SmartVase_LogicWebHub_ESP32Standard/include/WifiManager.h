/*! @file WifiManager.h
 *  @ingroup HubNetworking
 *  @brief Gestione della connessione Wi-Fi STA e del provisioning via Access
 *  Point + Captive Portal.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubNetworking Networking (Wi-Fi e MQTT)
 *  @brief Connettivita' verso la rete locale (Wi-Fi, provisioning) e verso il
 *  cloud (MQTT/TLS su HiveMQ).
 */

#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Libreria per il server web di provisioning
#include <DNSServer.h>          // Libreria per il Captive Portal DNS
#include "ConfigManager.h"     // Includi ConfigManager per accedere ai dati

/*! @addtogroup HubNetworking
 *  @{
 */

#define SSID_MAX_LENGTH         32  /**< Lunghezza massima SSID (standard WiFi) */
#define PASSWORD_MAX_LENGTH     64  /**< Lunghezza massima Password WiFi (standard WiFi) */
#define WIFI_CONNECT_TIMEOUT_MS 15000 /**< Timeout connessione WiFi (15 secondi) */

/**
 * @class WifiManager
 * @brief Gestore della connettività Wi-Fi e del Captive Portal per il provisioning dell'ESP32 Hub.
 *
 * Tenta di connettersi in modalità Station (STA) a partire dalle credenziali memorizzate in NVS.
 * Se la connessione fallisce o mancano le credenziali, avvia automaticamente un Access Point (AP)
 * protetto e un DNS Server che reindirizza tutte le richieste HTTP (Captive Portal) a una pagina di setup
 * web premium in glassmorphism (per inserire SSID e Password).
 *
 * @note Questo comportamento (avvio AP automatico su timeout/credenziali mancanti)
 * e' attivo dalla v1.3; in precedenza (v1.2) il fallimento di connessione NON
 * avviava l'AP e il provisioning si faceva solo da CLI seriale (vedi commento
 * storico su connect() in WifiManager.cpp).
 */
class WifiManager {
public:
    /**
     * @brief Costruttore della classe WifiManager.
     * @param configMgr Riferimento all'istanza di ConfigManager per leggere/scrivere la configurazione.
     */
    WifiManager(ConfigManager& configMgr);

    /**
     * @brief Tenta la connessione al Wi-Fi.
     * 
     * Se non riesce entro WIFI_CONNECT_TIMEOUT_MS o le credenziali non sono presenti, avvia l'AP di provisioning.
     */
    void connect();

    /**
     * @brief Ritorna se il dispositivo è connesso alla rete Wi-Fi (in modalità STA).
     * @return true se connesso con IP valido, false altrimenti.
     */
    bool isConnected() const;

    /**
     * @brief Gestisce le richieste del server DNS durante il Captive Portal.
     * 
     * Da chiamare ciclicamente nel loop del main o in un task dedicato se l'AP di provisioning è attivo.
     */
    void handleProvisioning();

private:
    ConfigManager& _configMgr;                /**< Riferimento al gestore della configurazione NVS */
    bool _isConnected;                        /**< Flag di connessione STA attiva */
    AsyncWebServer _provisioningServer;       /**< Server web asincrono per l'interfaccia HTML di configurazione */
    DNSServer _dnsServer;                     /**< Server DNS per intercettare il traffico e forzare il redirect */

    char _tempSsid[SSID_MAX_LENGTH];          /**< Buffer temporaneo per SSID */
    char _tempPassword[PASSWORD_MAX_LENGTH];  /**< Buffer temporaneo per password */

    /**
     * @brief Avvia l'Access Point locale "SmartVase_Setup_XXXX".
     */
    void startProvisioningAP();

    /**
     * @brief Registra le rotte HTTP del server di provisioning (HTML principale, salvataggio credenziali).
     */
    void setupProvisioningServer();

    /**
     * @brief Salva le nuove credenziali in NVS tramite il ConfigManager e riavvia l'ESP32.
     */
    void completeProvisioning();
};

/*! @} */ // end of HubNetworking group

#endif // WIFIMANAGER_H

