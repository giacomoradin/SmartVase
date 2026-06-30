#ifndef MAINLOGIC_H
#define MAINLOGIC_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "SerialManager.h"   // SerialMessage + WrapperMessage
#include "ConfigManager.h"

#define MQTT_TX_QUEUE_SIZE       15
#define MAX_JSON_MESSAGE_LENGTH  1024

typedef struct {
    char topic[64];
    char payload[MAX_JSON_MESSAGE_LENGTH];
} MqttMessage;

typedef struct {
    uint32_t timestamp;
    char     topic[64];
    char     payload[MAX_JSON_MESSAGE_LENGTH];
} MqttCommand;


class MainLogic {
public:
    MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
              QueueHandle_t mqttTxQueue,   QueueHandle_t mqttRxQueue,
              ConfigManager& configManager);

    static void taskEntry(void* pvParameters);
    void init();

    // --- Accessor per la CLI di debug (chiamati dal task loop()) ---
    bool isMegaConnected() const { return _isMegaConnected; }
    uint32_t megaLastMessageAgeMs() const { return millis() - _lastMegaHeartbeatMs; }
    const char* deviceId() const { return _deviceId; }
    // Copia coerente dell'ultima telemetria ricevuta dal Mega
    // (protetta da spinlock: il task MainLogic la aggiorna in concorrenza).
    void getTelemetrySnapshot(TelemetryFast& tf, TelemetryDeep& td);

private:
    QueueHandle_t _serialRxQueue;
    QueueHandle_t _serialTxQueue;
    QueueHandle_t _mqttTxQueue;
    QueueHandle_t _mqttRxQueue;
    ConfigManager& _configManager;

    uint32_t      _lastMegaHeartbeatMs;
    bool          _isMegaConnected;

    TelemetryFast _lastFastTelemetry;
    TelemetryDeep _lastDeepTelemetry;
    portMUX_TYPE  _telemetryMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t      _lastTelemetryPublishMs;

    char _deviceId[16];  // "HUB_XXXXXX"

    void taskRun();
    void processSerialMessage(const WrapperMessage& msg);
    void processMqttCommand(const MqttCommand& cmd);
    void publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td);
    void publishLogJson(const Log& log);
    void publishAlarmJson(const char* alarmType, const char* detail);
    void publishCommandAckJson(const CommandResponse& r);
    void sendCommandToMega(const Command& cmd);

    void checkMegaConnection();
};

#endif // MAINLOGIC_H
