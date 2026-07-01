/*! @file MainLogic.h
 *  @ingroup HubCore
 *  @brief Hub logic core: bridges the Mega's Protobuf protocol with the
 *  cloud's MQTT JSON, schedules telemetry/heartbeat, and runs the deadman switch.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubCore Logic core and bootstrap
 *  @brief JSON<->Protobuf bridge, FreeRTOS task scheduling and firmware entry
 *  point (main.cpp).
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
 * @brief Size of the MQTT transmit queue.
 */
#define MQTT_TX_QUEUE_SIZE       15

/**
 * @def MAX_JSON_MESSAGE_LENGTH
 * @brief Maximum allowed length of a JSON message (MQTT).
 */
#define MAX_JSON_MESSAGE_LENGTH  1024

/**
 * @struct MqttMessage
 * @brief Data structure holding an outgoing MQTT message (JSON payload).
 */
typedef struct {
    char topic[64];                           /**< MQTT topic to publish to */
    char payload[MAX_JSON_MESSAGE_LENGTH];    /**< JSON-formatted payload */
} MqttMessage;

/**
 * @struct MqttCommand
 * @brief Data structure representing an incoming MQTT command.
 */
typedef struct {
    uint32_t timestamp;                       /**< Reception timestamp */
    char     topic[64];                       /**< Source MQTT topic */
    char     payload[MAX_JSON_MESSAGE_LENGTH];/**< JSON command payload */
} MqttCommand;

/**
 * @class MainLogic
 * @brief Logic core of the ESP32 Hub. Coordinates the serial link (with the Mega) and Wi-Fi/MQTT (with the cloud).
 *
 * Handles routing of Protobuf messages coming from the Mega's serial link to the MQTT server (as JSON),
 * and the serialization/sending of cloud MQTT commands (JSON) into Protobuf messages forwarded to the Mega.
 * It also implements a "deadman switch" presence check to detect the Mega's physical disconnection.
 *
 * @note This is the body of the `TaskMainLogic` FreeRTOS task (Core 1, low
 * priority, isolated from the TLS load spikes of `TaskMqttLink` on Core 0).
 * Concurrent access to the cached telemetry (`_lastFastTelemetry`/`_lastDeepTelemetry`)
 * from the serial CLI is protected by `_telemetryMux` (see getTelemetrySnapshot()).
 */
class MainLogic {
public:
    /**
     * @brief MainLogic constructor.
     *
     * @param serialRxQueue FreeRTOS queue for messages received over serial (Mega -> Hub).
     * @param serialTxQueue FreeRTOS queue for messages to transmit over serial (Hub -> Mega).
     * @param mqttTxQueue FreeRTOS queue for messages to publish on MQTT.
     * @param mqttRxQueue FreeRTOS queue for commands received via MQTT subscription.
     * @param configManager Reference to the ConfigManager for NVS persistence.
     */
    MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
              QueueHandle_t mqttTxQueue,   QueueHandle_t mqttRxQueue,
              ConfigManager& configManager);

    /**
     * @brief Static entry point for the FreeRTOS task.
     * @param pvParameters Pointer to the MainLogic instance.
     */
    static void taskEntry(void* pvParameters);

    /**
     * @brief Initializes internal state and loads the base configuration.
     */
    void init();

    // --- Accessors for the debug CLI (called from the task loop()) ---
    /**
     * @brief Checks whether the active serial connection with the Arduino Mega is established.
     * @return true if the Mega responds to heartbeats, false otherwise.
     */
    bool isMegaConnected() const { return _isMegaConnected; }

    /**
     * @brief Returns the time elapsed since the last valid message received from the Arduino Mega.
     * @return uint32_t Time in milliseconds.
     */
    uint32_t megaLastMessageAgeMs() const { return millis() - _lastMegaHeartbeatMs; }

    /**
     * @brief Returns the Hub device's unique ID.
     * @return const char* Identifier string (e.g. "HUB_XXXXXX").
     */
    const char* deviceId() const { return _deviceId; }

    /**
     * @brief Obtains a mutex-protected snapshot of the latest telemetry received from the Mega.
     *
     * @param tf Output for the fast telemetry (TelemetryFast).
     * @param td Output for the deep telemetry (TelemetryDeep).
     */
    void getTelemetrySnapshot(TelemetryFast& tf, TelemetryDeep& td);

private:
    QueueHandle_t _serialRxQueue;             /**< Serial RX queue handle */
    QueueHandle_t _serialTxQueue;             /**< Serial TX queue handle */
    QueueHandle_t _mqttTxQueue;               /**< MQTT TX queue handle */
    QueueHandle_t _mqttRxQueue;               /**< MQTT RX queue handle */
    ConfigManager& _configManager;             /**< Reference to the configuration manager */

    uint32_t      _lastMegaHeartbeatMs;       /**< Timestamp of the last heartbeat from the Mega */
    bool          _isMegaConnected;           /**< Mega connection state */

    TelemetryFast _lastFastTelemetry;         /**< Cache of the last fast telemetry message */
    TelemetryDeep _lastDeepTelemetry;         /**< Cache of the last deep telemetry message */
    portMUX_TYPE  _telemetryMux = portMUX_INITIALIZER_UNLOCKED; /**< Mutex synchronizing telemetry data */
    uint32_t      _lastTelemetryPublishMs;    /**< Timestamp of the last telemetry publish to the MQTT broker */

    char _deviceId[16];                       /**< Hub device ID */

    /**
     * @brief Execution loop of the MainLogic task.
     */
    void taskRun();

    /**
     * @brief Processes a Protobuf message received over serial from the Mega.
     * @param msg Protobuf message to process.
     */
    void processSerialMessage(const WrapperMessage& msg);

    /**
     * @brief Processes an MQTT command received as JSON.
     * @param cmd Command to process.
     */
    void processMqttCommand(const MqttCommand& cmd);

    /**
     * @brief Converts and publishes telemetry as JSON to the MQTT broker.
     */
    void publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td);

    /**
     * @brief Converts and publishes a structured log as JSON to the MQTT broker.
     */
    void publishLogJson(const Log& log);

    /**
     * @brief Builds and publishes an alarm message to the MQTT broker.
     */
    void publishAlarmJson(const char* alarmType, const char* detail);

    /**
     * @brief Builds and publishes the acknowledgment (ACK) for a command to the MQTT broker.
     */
    void publishCommandAckJson(const CommandResponse& r);

    /**
     * @brief Sends a Protobuf command to the Arduino Mega over serial.
     */
    void sendCommandToMega(const Command& cmd);

    /**
     * @brief Checks the connection state with the Mega (deadman switch).
     */
    void checkMegaConnection();
};

/*! @} */ // end of HubCore group

#endif // MAINLOGIC_H
