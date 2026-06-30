#include "ConfigManager.h" // <-- LA RIGA FONDAMENTALE CHE MANCAVA!

#include "secrets.h"       // credenziali bench (gitignorato; vedi secrets.h.example)
#include "crc_utils.h"     // crc16_ibm (condiviso)
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h> // Per memset e memcpy
#include <stdint.h> // Per uint16_t etc.

// Tag per i log specifici di questo modulo
static const char *TAG = "ConfigManager";

// Namespace e Chiave per la NVS
#define NVS_NAMESPACE "smartvase_cfg"
#define NVS_CONFIG_KEY "device_config"

// Magic number per validare la struttura dati
#define CONFIG_MAGIC_NUMBER 0xCF6BEEF6

// CRC16 per l'integrita' dei blob NVS: IBM/ARC (poly 0xA001), implementazione
// condivisa in crc_utils. NB: e' DIVERSO dal CRC del protocollo seriale (CCITT,
// vedi SerialManager/Mega) ed e' corretto cosi' — sono due usi indipendenti.
namespace {
    // CRC dell'intera struct con il campo crc16 azzerato. Il campo crc16
    // sta in mezzo alla struct: calcolare il CRC con dentro il valore
    // vecchio (come faceva la versione precedente) rendeva impossibile la
    // verifica al load — la config NVS risultava sempre corrotta e le
    // credenziali venivano azzerate a ogni boot.
    uint16_t config_crc(const DeviceConfig& cfg) {
        DeviceConfig tmp = cfg;
        tmp.crc16 = 0;
        return crc16_ibm(reinterpret_cast<const uint8_t*>(&tmp), sizeof(DeviceConfig));
    }
}


// --- Implementazione Metodi Classe ConfigManager ---

ConfigManager::ConfigManager() {
    // Inizializza la configurazione in memoria con valori vuoti o di default sicuri
    memset(&_config, 0, sizeof(DeviceConfig));
    _config.magic_number = CONFIG_MAGIC_NUMBER; // Imposta subito il magic number
    strcpy(_config.wifi_ssid, "");
    strcpy(_config.wifi_password, "");
    strcpy(_config.mqtt_broker, "");
    _config.mqtt_port = 1883; // Porta MQTT standard
    strcpy(_config.mqtt_user, "");
    strcpy(_config.mqtt_password, "");
    strcpy(_config.webhook_url, "");
    // Aggiungi qui altri valori di default se necessario
}

bool ConfigManager::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Se la partizione NVS è corrotta o in una vecchia versione, la cancelliamo e reinizializziamo
        ESP_LOGW(TAG, "NVS partition damaged or outdated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init(); // Riprova l'inizializzazione
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
    // Inizializza loaded_config a zero per sicurezza prima della lettura
    memset(&loaded_config, 0, sizeof(DeviceConfig));
    size_t required_size = sizeof(DeviceConfig);
    err = nvs_get_blob(nvs_handle, NVS_CONFIG_KEY, &loaded_config, &required_size);

    bool load_successful = false;
    if (err == ESP_OK) {
        // Verifica Magic Number e Dimensione PRIMA di calcolare il CRC
        if (required_size == sizeof(DeviceConfig) && loaded_config.magic_number == CONFIG_MAGIC_NUMBER) {
            // CRC su una copia con il campo crc16 azzerato (vedi config_crc)
            uint16_t calculated_crc = config_crc(loaded_config);

            if (calculated_crc == loaded_config.crc16) {
                // CRC e Magic Number corrispondono, configurazione valida!
                memcpy(&_config, &loaded_config, sizeof(DeviceConfig));
                load_successful = true;
                ESP_LOGI(TAG, "Configuration loaded successfully from NVS.");
            } else {
                ESP_LOGW(TAG, "NVS CRC mismatch! Calculated=0x%X, Stored=0x%X", calculated_crc, loaded_config.crc16);
            }
        } else {
             // Stampa valori specifici per il debug
             ESP_LOGW(TAG, "NVS magic/size mismatch! Read Size=%d, Expected Size=%d, Read Magic=0x%lX, Expected Magic=0x%lX",
                     required_size, sizeof(DeviceConfig), (unsigned long)loaded_config.magic_number, (unsigned long)CONFIG_MAGIC_NUMBER);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Configuration key '%s' not found in NVS.", NVS_CONFIG_KEY);
    } else {
        ESP_LOGE(TAG, "Error reading configuration blob from NVS (%s)", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);

    // NVS-first: se la config e' valida E contiene credenziali Wi-Fi, usala.
    // Cosi' il provisioning via CLI (`set` + `save`) viene rispettato dopo un
    // riavvio (era rotto dal vecchio bypass bench che ritornava subito).
    bool provisioned = load_successful && _config.wifi_ssid[0] != '\0';
    if (provisioned) {
        ESP_LOGI(TAG, "Configuration loaded from NVS (provisioned).");
        return true;
    }

    // NVS vuota / invalida / senza Wi-Fi: parti da default puliti.
    memset(&_config, 0, sizeof(DeviceConfig));
    _config.magic_number = CONFIG_MAGIC_NUMBER;
    _config.mqtt_port    = 1883;

#ifdef SV_BENCH_MODE
    // Fallback BENCH: credenziali da secrets.h (gitignorato) per collaudo
    // immediato senza provisioning. Tenute solo in RAM (NON salvate): editare
    // secrets.h ha effetto a ogni boot, e un `set`+`save` da CLI passa
    // stabilmente alla NVS (che da quel momento vince, vedi 'provisioned').
    setWifiCredentials(SV_WIFI_SSID, SV_WIFI_PASS);
    setMqttConfig(SV_MQTT_BROKER, SV_MQTT_PORT, SV_MQTT_USER, SV_MQTT_PASS);
    ESP_LOGW(TAG, "NVS vuota: uso credenziali di fallback da secrets.h (SV_BENCH_MODE).");
    return true;
#else
    // Produzione: nessuna credenziale -> provisioning via CLI o AP. Inizializza NVS.
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

     // Assicura che il magic number sia corretto prima di salvare (anche se dovrebbe già esserlo)
    _config.magic_number = CONFIG_MAGIC_NUMBER;

    // CRC su una copia con il campo crc16 azzerato (vedi config_crc)
    _config.crc16 = config_crc(_config);

    // Scrive l'intera struttura come blob
    err = nvs_set_blob(nvs_handle, NVS_CONFIG_KEY, &_config, sizeof(DeviceConfig));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing configuration blob to NVS (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Effettua il commit delle modifiche sulla NVS
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

// --- Metodi Getter ---

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

// --- Metodi Setter (per aggiornare la configurazione in memoria prima di salvarla) ---

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

