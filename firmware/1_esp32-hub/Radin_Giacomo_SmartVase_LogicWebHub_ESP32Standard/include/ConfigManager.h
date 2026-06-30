/*! @file ConfigManager.h
 *  @ingroup HubConfig
 *  @brief Gestione della configurazione persistente dell'Hub (NVS/Preferences).
 *  @details Definisce la struct `DeviceConfig` (credenziali Wi-Fi, broker MQTT,
 *  webhook) salvata come blob in NVS con magic number + CRC16-IBM per
 *  rilevare corruzione, e la classe `ConfigManager` che la carica/salva e ne
 *  espone getter/setter. E' la fonte di verita' usata da `WifiManager`,
 *  `MqttManager`, `MainLogic` e `HubCli`.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubConfig Configurazione (NVS)
 *  @brief Persistenza della configurazione del dispositivo (Wi-Fi, MQTT, webhook)
 *  su NVS, con validazione tramite magic number e CRC16.
 */

#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <Arduino.h> // Per tipi standard come uint16_t, size_t
#include <stdint.h>  // Per uint32_t etc.

/*! @addtogroup HubConfig
 *  @{
 */

/*! @name Lunghezze massime dei campi di configurazione (incluso terminatore NUL)
 *  @{
 */
#define WIFI_SSID_MAX_LENGTH     32  /**< Lunghezza massima SSID Wi-Fi. */
#define WIFI_PASSWORD_MAX_LENGTH 64  /**< Lunghezza massima password Wi-Fi. */
#define MQTT_BROKER_MAX_LENGTH   64  /**< Lunghezza massima hostname/URL broker MQTT. */
#define MQTT_USER_MAX_LENGTH     32  /**< Lunghezza massima username MQTT. */
#define MQTT_PASSWORD_MAX_LENGTH 32  /**< Lunghezza massima password MQTT. */
#define WEBHOOK_URL_MAX_LENGTH   128 /**< Lunghezza massima URL webhook opzionale. */
/*! @} */

/*!
 * @def HUB_DEVICE_ID
 * @brief ID statico del dispositivo Hub, usato in tutti i topic MQTT e nei
 * messaggi protobuf verso il Mega.
 * @note Scelta del team (allineamento con Firebase): NON va derivato dal MAC
 * address, a differenza del client ID MQTT (vedi `MqttManager::init`).
 */
#define HUB_DEVICE_ID "HUB_123456"

/*!
 * @struct DeviceConfig
 * @brief Layout binario di TUTTI i dati di configurazione persistiti come
 * singolo blob in NVS (namespace `smartvase_cfg`, chiave `device_config`).
 * @note La struct viene scritta/letta per intero con `nvs_set_blob`/`nvs_get_blob`:
 * qualsiasi modifica ai campi invalida i blob gia' salvati (vince comunque il
 * controllo magic+CRC, che fa ripartire da default puliti).
 */
struct DeviceConfig {
    uint32_t magic_number;            /**< Magic number per riconoscere un blob valido (vedi CONFIG_MAGIC_NUMBER in ConfigManager.cpp). */
    uint16_t crc16;                   /**< CRC16-IBM calcolato sull'intera struct con questo campo azzerato (vedi config_crc()). */

    // Credenziali WiFi
    char wifi_ssid[WIFI_SSID_MAX_LENGTH];         /**< SSID della rete Wi-Fi STA, stringa NUL-terminated. */
    char wifi_password[WIFI_PASSWORD_MAX_LENGTH]; /**< Password della rete Wi-Fi STA, stringa NUL-terminated. */

    // Configurazione MQTT
    char mqtt_broker[MQTT_BROKER_MAX_LENGTH];     /**< Hostname o IP del broker MQTT. */
    uint16_t mqtt_port;                           /**< Porta del broker: 8883/8884 = TLS, 1883 = plaintext (vedi MqttManager::init). */
    char mqtt_user[MQTT_USER_MAX_LENGTH];         /**< Username per l'autenticazione MQTT. */
    char mqtt_password[MQTT_PASSWORD_MAX_LENGTH]; /**< Password per l'autenticazione MQTT. */

    // Altre configurazioni
    char webhook_url[WEBHOOK_URL_MAX_LENGTH];     /**< URL webhook opzionale (non ancora utilizzato dal resto del firmware). */
};

/*!
 * @class ConfigManager
 * @brief Carica e salva `DeviceConfig` su NVS, espone i valori correnti
 * tramite getter/setter alle altre classi (WifiManager, MqttManager, HubCli).
 * @note Non e' thread-safe: e' usato solo dal task Arduino principale
 * (setup()/loop(), inclusa la CLI) e mai da TaskMqttLink/TaskMainLogic
 * direttamente (quei task leggono solo riferimenti/copie passate al costruttore).
 */
class ConfigManager {
public:
    /*! @brief Costruttore: inizializza `_config` con valori di default sicuri
     *  (stringhe vuote, porta MQTT 1883) finche' loadConfig() non viene chiamato. */
    ConfigManager();

    /*! @brief Inizializza il sottosistema NVS sottostante (`nvs_flash_init`).
     *  @details Se la partizione NVS risulta corrotta o di una vecchia versione
     *  la cancella e la reinizializza automaticamente.
     *  @return true se la NVS e' pronta all'uso, false in caso di errore irreversibile.
     *  @note Da chiamare una sola volta in setup(), prima di loadConfig(). */
    bool init();

    /*! @brief Carica la configurazione dalla NVS in memoria (`_config`).
     *  @details Verifica magic number, dimensione e CRC16 del blob letto. Se il
     *  blob e' valido e contiene un SSID Wi-Fi non vuoto ("provisioned"), lo usa
     *  cosi' com'e' (NVS-first). Altrimenti riparte da default puliti e, se
     *  compilato con `SV_BENCH_MODE`, popola le credenziali da `secrets.h`
     *  SOLO in RAM (non salvate su NVS) per consentire il collaudo a banco senza
     *  provisioning manuale.
     *  @return true se la configurazione finale (NVS o fallback bench) e' utilizzabile,
     *  false solo in caso di errore di accesso NVS.
     *  @note In assenza di `SV_BENCH_MODE` e di credenziali valide, scrive comunque
     *  i default su NVS (saveConfig()) cosi' che il provisioning successivo trovi
     *  un blob con magic/CRC coerenti. */
    bool loadConfig();

    /*! @brief Salva la configurazione corrente in memoria (`_config`) sulla NVS.
     *  @details Ricalcola magic number e CRC16 prima della scrittura del blob.
     *  @return true se scrittura e commit su NVS sono andati a buon fine.
     *  @note Operazione bloccante (accesso flash); va chiamata solo da contesti
     *  non time-critical (CLI `save`, fine provisioning AP). */
    bool saveConfig();

    // --- Metodi Getter per accedere ai valori di configurazione ---
    /*! @brief @return SSID Wi-Fi corrente (stringa vuota se non configurato). */
    const char* getWifiSsid() const;
    /*! @brief @return Password Wi-Fi corrente. */
    const char* getWifiPassword() const;
    /*! @brief @return Hostname/IP del broker MQTT corrente. */
    const char* getMqttBroker() const;
    /*! @brief @return Porta del broker MQTT corrente. */
    uint16_t    getMqttPort() const;
    /*! @brief @return Username MQTT corrente. */
    const char* getMqttUser() const;
    /*! @brief @return Password MQTT corrente. */
    const char* getMqttPassword() const;
    /*! @brief @return URL webhook corrente (non ancora utilizzato). */
    const char* getWebhookUrl() const;

    // --- Metodi Setter per modificare la configurazione in memoria ---
    /*! @brief Imposta le credenziali Wi-Fi in memoria.
     *  @param[in] ssid SSID da impostare; se nullptr la stringa viene svuotata.
     *  @param[in] password Password da impostare; se nullptr la stringa viene svuotata.
     *  @note Non persiste su NVS: chiamare saveConfig() per rendere la modifica
     *  sopravvivente al reboot. I valori sono troncati alla lunghezza massima
     *  del campo (WIFI_SSID_MAX_LENGTH/WIFI_PASSWORD_MAX_LENGTH - 1). */
    void setWifiCredentials(const char* ssid, const char* password);
    /*! @brief Imposta in blocco la configurazione MQTT in memoria (broker, porta, utente, password).
     *  @param[in] broker Hostname/IP del broker; nullptr svuota il campo.
     *  @param[in] port Porta del broker (8883/8884 = TLS, 1883 = plaintext).
     *  @param[in] user Username MQTT; nullptr svuota il campo.
     *  @param[in] password Password MQTT; nullptr svuota il campo.
     *  @note Non persiste su NVS: chiamare saveConfig() per rendere la modifica
     *  sopravvivente al reboot. */
    void setMqttConfig(const char* broker, uint16_t port, const char* user, const char* password);
    /*! @brief Imposta l'URL webhook in memoria.
     *  @param[in] url URL da impostare; se nullptr la stringa viene svuotata.
     *  @note Non persiste su NVS: chiamare saveConfig() per rendere la modifica
     *  sopravvivente al reboot. */
    void setWebhookUrl(const char* url);

private:
    DeviceConfig _config; /**< Configurazione corrente in RAM, sincronizzata su NVS da loadConfig()/saveConfig(). */
};

/*! @} */ // end of HubConfig group

#endif // CONFIGMANAGER_H
