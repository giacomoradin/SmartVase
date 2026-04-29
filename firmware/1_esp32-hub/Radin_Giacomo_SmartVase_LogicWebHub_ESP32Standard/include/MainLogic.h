#ifndef MAINLOGIC_H
#define MAINLOGIC_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h" // Per i timer software (es. telemetria)

// Include le definizioni necessarie
#include "SerialManager.h" // Per la struct SerialMessage e WrapperMessage
#include "ConfigManager.h" // Per accedere alla configurazione (es. URL webhook indiretto)

// Dimensione della coda per i messaggi JSON da inviare a MQTT
#define MQTT_TX_QUEUE_SIZE 15
// Dimensione massima (stimata) di un messaggio JSON (telemetria, log, allarme)
#define MAX_JSON_MESSAGE_LENGTH 512 // Aumentare se necessario

// Struttura per i messaggi JSON da inviare via MQTT
typedef struct {
    char topic[64]; // Topic MQTT di destinazione (es. "smartvase/HUB01/telemetry")
    char payload[MAX_JSON_MESSAGE_LENGTH]; // Payload JSON
} MqttMessage;

// Struttura per i comandi JSON ricevuti via MQTT
typedef struct {
    uint32_t timestamp; // Timestamp di ricezione
    char topic[64];     // Topic da cui è arrivato il messaggio
    char payload[MAX_JSON_MESSAGE_LENGTH]; // Payload JSON del comando
} MqttCommand;


class MainLogic {
public:
    // Costruttore: Riceve le code di comunicazione e il ConfigManager
    MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
              QueueHandle_t mqttTxQueue, QueueHandle_t mqttRxQueue, ConfigManager& configManager);

    // Funzione statica entry point per il Task FreeRTOS
    static void taskEntry(void* pvParameters);

    // Inizializza eventuali timer o risorse necessarie
    void init();

private:
    // Code FreeRTOS
    QueueHandle_t _serialRxQueue; // Riceve Protobuf decodificati dal Mega
    QueueHandle_t _serialTxQueue; // Invia comandi Protobuf al Mega
    QueueHandle_t _mqttTxQueue;   // Invia JSON al MqttManager
    QueueHandle_t _mqttRxQueue;   // Riceve comandi JSON dal MqttManager

    // Riferimento al ConfigManager
    ConfigManager& _configManager;

    // Timer FreeRTOS per l'invio periodico della telemetria
    TimerHandle_t _telemetryTimer;

    // Ultimo timestamp heartbeat ricevuto dal Mega (per deadman switch)
    uint32_t _lastMegaHeartbeatMs;
    bool _isMegaConnected;

    // Stato interno (esempio: ultimo valore batteria)
    float _lastBatteryVoltage;

    // Cache per gli ultimi dati di telemetria ricevuti dal Mega
    TelemetryFast _lastFastTelemetry;
    TelemetryDeep _lastDeepTelemetry;

    // Funzione principale del Task (loop infinito)
    void taskRun();

    // Gestisce un messaggio Protobuf ricevuto dal Mega
    void processSerialMessage(const WrapperMessage& msg);

    // Gestisce un comando JSON ricevuto via MQTT
    void processMqttCommand(const MqttCommand& cmd);

    // Converte la telemetria Protobuf in JSON e la invia alla coda MQTT
    void publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td);

    // Converte un log Protobuf in JSON e lo invia alla coda MQTT
    void publishLogJson(const Log& log);

    // Invia un allarme JSON alla coda MQTT
    void publishAlarmJson(const char* alarmType, const char* detail);

    // Invia un comando Protobuf al Mega (lo mette nella coda serialTxQueue)
    void sendCommandToMega(const Command& cmd);

    // Callback statico per il timer della telemetria
    static void telemetryTimerCallback(TimerHandle_t xTimer);

    // Controlla il deadman switch del Mega
    void checkMegaConnection();

    // Implementa la logica di default per la pianta (placeholder)
    void applyDefaultPlantLogic();
};

#endif // MAINLOGIC_H
