/*! @file MainLogic.h
 *  @ingroup HubCore
 *  @brief Core logico dell'Hub: bridge tra il protocollo Protobuf del Mega e
 *  il JSON MQTT del cloud, scheduling telemetria/heartbeat, deadman switch.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubCore Core logico e bootstrap
 *  @brief Bridge JSON<->Protobuf, scheduling dei task FreeRTOS e punto di
 *  ingresso del firmware (main.cpp).
 */

#ifndef MAINLOGIC_H
#define MAINLOGIC_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "SerialManager.h"   // SerialMessage + WrapperMessage
#include "ConfigManager.h"

/*! @addtogroup HubCore
 *  @{
 */

/**
 * @def MQTT_TX_QUEUE_SIZE
 * @brief Dimensione della coda di trasmissione MQTT.
 */
#define MQTT_TX_QUEUE_SIZE       15

/**
 * @def MAX_JSON_MESSAGE_LENGTH
 * @brief Lunghezza massima consentita per un messaggio JSON (MQTT).
 */
#define MAX_JSON_MESSAGE_LENGTH  1024

/**
 * @struct MqttMessage
 * @brief Struttura dati per contenere un messaggio MQTT in uscita (Payload JSON).
 */
typedef struct {
    char topic[64];                           /**< Topic MQTT su cui pubblicare */
    char payload[MAX_JSON_MESSAGE_LENGTH];    /**< Payload in formato JSON */
} MqttMessage;

/**
 * @struct MqttCommand
 * @brief Struttura dati per rappresentare un comando MQTT ricevuto in ingresso.
 */
typedef struct {
    uint32_t timestamp;                       /**< Timestamp di ricezione */
    char     topic[64];                       /**< Topic MQTT di provenienza */
    char     payload[MAX_JSON_MESSAGE_LENGTH];/**< Payload del comando JSON */
} MqttCommand;

/**
 * @class MainLogic
 * @brief Core logico dell'ESP32 Hub. Coordina la seriale (con il Mega) e il Wi-Fi/MQTT (con il cloud).
 *
 * Gestisce l'instradamento dei messaggi Protobuf provenienti dalla seriale del Mega verso il server MQTT (in JSON),
 * e la serializzazione/invio dei comandi cloud MQTT (in JSON) in messaggi Protobuf da inoltrare al Mega.
 * Implementa inoltre un controllo di presenza "deadman switch" per rilevare la disconnessione fisica del Mega.
 *
 * @note E' il corpo del task FreeRTOS `TaskMainLogic` (Core 1, priorita' bassa,
 * isolato dai picchi di carico TLS di `TaskMqttLink` su Core 0). L'accesso
 * concorrente alle telemetrie cache (`_lastFastTelemetry`/`_lastDeepTelemetry`)
 * da parte della CLI seriale e' protetto da `_telemetryMux` (vedi getTelemetrySnapshot()).
 */
class MainLogic {
public:
    /**
     * @brief Costruttore di MainLogic.
     * 
     * @param serialRxQueue Coda FreeRTOS per i messaggi ricevuti dalla seriale (Mega -> Hub).
     * @param serialTxQueue Coda FreeRTOS per i messaggi da trasmettere sulla seriale (Hub -> Mega).
     * @param mqttTxQueue Coda FreeRTOS per i messaggi da pubblicare su MQTT.
     * @param mqttRxQueue Coda FreeRTOS per i comandi ricevuti tramite sottoscrizione MQTT.
     * @param configManager Riferimento al ConfigManager per la persistenza su NVS.
     */
    MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
              QueueHandle_t mqttTxQueue,   QueueHandle_t mqttRxQueue,
              ConfigManager& configManager);

    /**
     * @brief Entry point statico per il task FreeRTOS.
     * @param pvParameters Puntatore all'istanza di MainLogic.
     */
    static void taskEntry(void* pvParameters);

    /**
     * @brief Inizializza gli stati interni e carica le configurazioni di base.
     */
    void init();

    // --- Accessor per la CLI di debug (chiamati dal task loop()) ---
    /**
     * @brief Verifica se la connessione seriale attiva con l'Arduino Mega è stabilita.
     * @return true se il Mega risponde agli heartbeat, false altrimenti.
     */
    bool isMegaConnected() const { return _isMegaConnected; }

    /**
     * @brief Restituisce il tempo trascorso dall'ultimo messaggio valido ricevuto dall'Arduino Mega.
     * @return uint32_t Tempo in millisecondi.
     */
    uint32_t megaLastMessageAgeMs() const { return millis() - _lastMegaHeartbeatMs; }

    /**
     * @brief Restituisce l'ID univoco del dispositivo Hub.
     * @return const char* Stringa identificativa (es. "HUB_XXXXXX").
     */
    const char* deviceId() const { return _deviceId; }

    /**
     * @brief Ottiene uno snapshot protetto da mutex delle ultime telemetrie ricevute dal Mega.
     * 
     * @param tf Output per la telemetria veloce (TelemetryFast).
     * @param td Output per la telemetria profonda (TelemetryDeep).
     */
    void getTelemetrySnapshot(TelemetryFast& tf, TelemetryDeep& td);

private:
    QueueHandle_t _serialRxQueue;             /**< Handle della coda RX seriale */
    QueueHandle_t _serialTxQueue;             /**< Handle della coda TX seriale */
    QueueHandle_t _mqttTxQueue;               /**< Handle della coda TX MQTT */
    QueueHandle_t _mqttRxQueue;               /**< Handle della coda RX MQTT */
    ConfigManager& _configManager;             /**< Riferimento al gestore configurazioni */

    uint32_t      _lastMegaHeartbeatMs;       /**< Timestamp dell'ultimo heartbeat del Mega */
    bool          _isMegaConnected;           /**< Stato connessione Mega */

    TelemetryFast _lastFastTelemetry;         /**< Cache dell'ultimo messaggio di telemetria veloce */
    TelemetryDeep _lastDeepTelemetry;         /**< Cache dell'ultimo messaggio di telemetria profonda */
    portMUX_TYPE  _telemetryMux = portMUX_INITIALIZER_UNLOCKED; /**< Mutex per la sincronizzazione dei dati di telemetria */
    uint32_t      _lastTelemetryPublishMs;    /**< Timestamp dell'ultimo invio di telemetria al broker MQTT */

    char _deviceId[16];                       /**< ID del dispositivo Hub */

    /**
     * @brief Loop di esecuzione del task MainLogic.
     */
    void taskRun();

    /**
     * @brief Elabora un messaggio Protobuf ricevuto sulla seriale dal Mega.
     * @param msg Messaggio Protobuf da elaborare.
     */
    void processSerialMessage(const WrapperMessage& msg);

    /**
     * @brief Elabora un comando MQTT ricevuto in formato JSON.
     * @param cmd Comando da elaborare.
     */
    void processMqttCommand(const MqttCommand& cmd);

    /**
     * @brief Converte e pubblica in formato JSON le telemetrie sul broker MQTT.
     */
    void publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td);

    /**
     * @brief Converte e pubblica in formato JSON un log strutturato sul broker MQTT.
     */
    void publishLogJson(const Log& log);

    /**
     * @brief Genera e pubblica un messaggio di allarme sul broker MQTT.
     */
    void publishAlarmJson(const char* alarmType, const char* detail);

    /**
     * @brief Genera e pubblica la risposta ad un comando (ACK) sul broker MQTT.
     */
    void publishCommandAckJson(const CommandResponse& r);

    /**
     * @brief Invia un comando Protobuf all'Arduino Mega tramite la seriale.
     */
    void sendCommandToMega(const Command& cmd);

    /**
     * @brief Controlla lo stato di connessione con il Mega (deadman switch).
     */
    void checkMegaConnection();
};

/*! @} */ // end of HubCore group

#endif // MAINLOGIC_H
