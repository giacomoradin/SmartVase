/*! @file MQTTManager.h
 *  @ingroup HubNetworking
 *  @brief Client MQTT verso HiveMQ Cloud: pubblica telemetria/log/alarm/ack e
 *  riceve i comandi sottoscritti su `smartvase/{device_id}/command/#`.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#ifndef MQTTMANAGER_H
#define MQTTMANAGER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <WiFi.h>             // WiFiClient (plaintext)
#include <WiFiClientSecure.h> // Per connessione TLS
#include <PubSubClient.h>     // Libreria MQTT

// Include le definizioni necessarie
#include "ConfigManager.h" // Per accedere alla configurazione MQTT
#include "MainLogic.h"     // Per la struct MqttMessage (definita lì)

/*! @addtogroup HubNetworking
 *  @{
 */

/*!
 * @def MQTT_BUFFER_SIZE
 * @brief Dimensione del buffer interno di PubSubClient per i messaggi
 * MQTT in ingresso/uscita.
 * @note Deve restare >= MAX_JSON_MESSAGE_LENGTH + overhead del topic, altrimenti
 * PubSubClient tronca silenziosamente i payload piu' grandi.
 */
#define MQTT_BUFFER_SIZE (MAX_JSON_MESSAGE_LENGTH + 128)

/*!
 * @class MqttManager
 * @brief Gestisce la connessione MQTT/TLS al broker HiveMQ Cloud: corpo del
 * task FreeRTOS `TaskMqttLink` (vedi taskRun()).
 * @details Consuma `_txQueue` (JSON pronti da pubblicare, prodotti da MainLogic)
 * e produce su `_rxQueue` i comandi ricevuti dalla sottoscrizione
 * `command/#` (consumati da MainLogic). Sceglie il transport (plaintext o TLS)
 * in base alla porta configurata: 8883/8884 = TLS con verifica CA HiveMQ e
 * NTP obbligatorio, 1883 = plaintext senza NTP (pensato per un broker locale
 * in laboratorio offline).
 * @note Le funzioni isConfigured()/isConnected() sono lette anche dalla CLI
 * seriale (task Arduino principale) senza sincronizzazione esplicita con
 * TaskMqttLink: i valori restituiti sono quindi indicativi, va bene per il
 * solo scopo di debug/diagnostica.
 */
class MqttManager {
public:
    /*! @brief Costruttore.
     *  @param[in] txQueue Coda FreeRTOS da cui questo task legge i JSON da pubblicare.
     *  @param[in] rxQueue Coda FreeRTOS su cui questo task scrive i comandi ricevuti via MQTT.
     *  @param[in] configManager Riferimento al ConfigManager per leggere broker/porta/credenziali. */
    MqttManager(QueueHandle_t txQueue, QueueHandle_t rxQueue, ConfigManager& configManager);

    /*! @brief Entry point statico per `xTaskCreatePinnedToCore`: inoltra alla taskRun() dell'istanza.
     *  @param[in] pvParameters Puntatore alla MqttManager su cui girare il task. */
    static void taskEntry(void* pvParameters);

    /*! @brief Inizializza client ID, topic, transport (TLS/plaintext) e parametri
     *  di PubSubClient.
     *  @details Genera il client ID MQTT dal MAC (`SmartVase_HUB_<MAC>`), calcola
     *  i topic `command/#` e `status` da `HUB_DEVICE_ID`, e imposta il CA cert
     *  HiveMQ solo se la porta configurata richiede TLS (8883/8884).
     *  @note Va chiamata una sola volta da setup(), PRIMA della creazione del
     *  task TaskMqttLink (taskEntry() non la richiama, per evitare doppia init). */
    void init();

    // --- Stato per la CLI di debug ---
    /*! @brief @return true se un broker MQTT non vuoto e' configurato in NVS.
     *  @note Lettura non sincronizzata col task MQTT: valore indicativo, solo per debug/CLI. */
    bool isConfigured() {
        const char* b = _configManager.getMqttBroker();
        return b != nullptr && strlen(b) > 0;
    }
    /*! @brief @return true se il client PubSubClient risulta connesso al broker.
     *  @note Lettura non sincronizzata col task MQTT: valore indicativo, solo per debug/CLI. */
    bool isConnected() { return _mqttClient.connected(); }

private:
    QueueHandle_t _txQueue; /**< Coda per ricevere JSON da pubblicare (prodotta da MainLogic). */
    QueueHandle_t _rxQueue; /**< Coda per inoltrare a MainLogic i comandi ricevuti via MQTT. */

    ConfigManager& _configManager; /**< Riferimento al gestore configurazioni (broker, credenziali). */

    // Transport WiFi: plaintext per broker su porta 1883, TLS per 8883/8884.
    WiFiClient       _wifiClient;       /**< Transport plaintext, usato se la porta configurata e' 1883. */
    WiFiClientSecure _wifiClientSecure; /**< Transport TLS (CA HiveMQ), usato per le porte 8883/8884. */
    PubSubClient     _mqttClient;       /**< Client MQTT (libreria PubSubClient) sopra uno dei due transport. */

    char _mqttBuffer[MQTT_BUFFER_SIZE]; /**< Buffer interno di PubSubClient per i frame MQTT. */

    String _mqttClientId; /**< ID univoco del client MQTT, derivato dal MAC Wi-Fi STA. */
    String _topicCommand; /**< Topic di sottoscrizione comandi, es. smartvase/HUB_123456/command/#. */
    String _topicStatus;  /**< Topic di stato/LWT, es. smartvase/HUB_123456/status. */

    /*! @brief Corpo del task FreeRTOS `TaskMqttLink`: loop infinito che gestisce
     *  connessione, mantenimento (PubSubClient::loop()) e pubblicazione/ricezione messaggi.
     *  @details Se il broker non e' configurato, drena e scarta la coda TX senza
     *  tentare connessioni. Se disconnesso, tenta reconnect() e nel frattempo
     *  scarta dalla coda TX solo i messaggi di telemetria (perdono valore se
     *  vecchi), mantenendo invece alarm/log/ack per quando torna online. */
    void taskRun();

    /*! @brief Tenta la (ri)connessione al broker MQTT, NON bloccante.
     *  @details Se serve TLS (porta 8883/8884) e l'ora di sistema non e' ancora
     *  valida (NTP non sincronizzato), rimanda il connect e forza un nuovo
     *  tentativo SNTP ogni 30 s anziche' attendere il retry orario di default
     *  della libreria SNTP. Applica poi un backoff esponenziale (5->10->20->30 s)
     *  con jitter tra i tentativi di connect veri e propri.
     *  @return true se la connessione (o riconnessione) e' andata a buon fine.
     *  @note "Non bloccante" qui significa che ritorna subito se il backoff non
     *  e' ancora scaduto: la cadenza delle chiamate la da' il vTaskDelay in taskRun(). */
    bool reconnect();

    /*! @brief Callback di PubSubClient invocata per ogni messaggio in arrivo sui topic sottoscritti.
     *  @param[in] topic Topic MQTT del messaggio ricevuto.
     *  @param[in] payload Buffer del payload (non NUL-terminated).
     *  @param[in] length Lunghezza del payload in byte.
     *  @note Deve essere statica (vincolo della libreria PubSubClient): usa
     *  `_instance` per risalire allo stato dell'oggetto. Payload >= sizeof(MqttCommand::payload)
     *  vengono scartati con un log di errore. */
    static void mqttCallback(char* topic, byte* payload, unsigned int length);

    static MqttManager* _instance; /**< Puntatore statico all'unica istanza, usato dal callback statico mqttCallback(). */
};

/*! @} */ // end of HubNetworking group

#endif // MQTTMANAGER_H
