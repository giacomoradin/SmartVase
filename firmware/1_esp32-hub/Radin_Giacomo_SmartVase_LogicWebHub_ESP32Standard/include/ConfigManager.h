#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <Arduino.h> // Per tipi standard come uint16_t, size_t
#include <stdint.h>  // Per uint32_t etc.

// Definisci qui le lunghezze massime per le stringhe di configurazione
#define WIFI_SSID_MAX_LENGTH     32
#define WIFI_PASSWORD_MAX_LENGTH 64
#define MQTT_BROKER_MAX_LENGTH   64
#define MQTT_USER_MAX_LENGTH     32
#define MQTT_PASSWORD_MAX_LENGTH 32
#define WEBHOOK_URL_MAX_LENGTH   128

// Struttura che contiene TUTTI i dati di configurazione salvati su NVS
struct DeviceConfig {
    uint32_t magic_number;            // Per verificare se i dati sono validi
    uint16_t crc16;                   // Checksum per l'integrità dei dati

    // Credenziali WiFi
    char wifi_ssid[WIFI_SSID_MAX_LENGTH];
    char wifi_password[WIFI_PASSWORD_MAX_LENGTH];

    // Configurazione MQTT
    char mqtt_broker[MQTT_BROKER_MAX_LENGTH];
    uint16_t mqtt_port;
    char mqtt_user[MQTT_USER_MAX_LENGTH];
    char mqtt_password[MQTT_PASSWORD_MAX_LENGTH];

    // Altre configurazioni
    char webhook_url[WEBHOOK_URL_MAX_LENGTH];

    // Aggiungi qui altri campi di configurazione se necessario...
};

// Classe per gestire la lettura e scrittura della configurazione su NVS
class ConfigManager {
public:
    // Costruttore
    ConfigManager();

    // Inizializza il sottosistema NVS (da chiamare nel setup)
    bool init();

    // Carica la configurazione dalla NVS alla memoria (_config).
    // Se non valida o non presente, carica i default e li salva.
    bool loadConfig();

    // Salva la configurazione corrente in memoria (_config) sulla NVS.
    bool saveConfig();

    // --- Metodi Getter per accedere ai valori di configurazione ---
    const char* getWifiSsid() const;
    const char* getWifiPassword() const;
    const char* getMqttBroker() const;
    uint16_t    getMqttPort() const;
    const char* getMqttUser() const;
    const char* getMqttPassword() const;
    const char* getWebhookUrl() const;
    // Aggiungi altri getter se necessario...

    // --- Metodi Setter per modificare la configurazione in memoria ---
    // NOTA: Chiamare saveConfig() dopo i setter per rendere le modifiche persistenti.
    void setWifiCredentials(const char* ssid, const char* password);
    void setMqttConfig(const char* broker, uint16_t port, const char* user, const char* password);
    void setWebhookUrl(const char* url);
    // Aggiungi altri setter se necessario...

private:
    // Istanza della struttura che contiene la configurazione corrente in RAM
    DeviceConfig _config;
};

#endif // CONFIGMANAGER_H
