/*! @file MQTTManager.h
 *  @ingroup HubNetworking
 *  @brief MQTT client towards HiveMQ Cloud: publishes telemetry/log/alarm/ack
 *  and receives the commands subscribed on `smartvase/{device_id}/command/#`.
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
#include <WiFiClientSecure.h> // For the TLS connection
#include <PubSubClient.h>     // MQTT library

// Include the required definitions
#include "ConfigManager.h" // To access the MQTT configuration
#include "MainLogic.h"     // For the MqttMessage struct (defined there)

/*! @addtogroup HubNetworking
 *  @{
 */

/*!
 * @def MQTT_BUFFER_SIZE
 * @brief Size of the internal PubSubClient buffer for the inbound/outbound
 * MQTT messages.
 * @note Must stay >= MAX_JSON_MESSAGE_LENGTH + topic overhead, otherwise
 * PubSubClient silently truncates the larger payloads.
 */
#define MQTT_BUFFER_SIZE (MAX_JSON_MESSAGE_LENGTH + 128)

/*!
 * @class MqttManager
 * @brief Manages the MQTT/TLS connection to the HiveMQ Cloud broker: body of
 * the FreeRTOS task `TaskMqttLink` (see taskRun()).
 * @details Consumes `_txQueue` (JSON ready to publish, produced by MainLogic)
 * and produces on `_rxQueue` the commands received from the `command/#`
 * subscription (consumed by MainLogic). It chooses the transport (plaintext or
 * TLS) based on the configured port: 8883/8884 = TLS with HiveMQ CA
 * verification and mandatory NTP, 1883 = plaintext without NTP (intended for a
 * local broker in an offline lab).
 * @note The isConfigured()/isConnected() functions are also read from the
 * serial CLI (main Arduino task) without explicit synchronization with
 * TaskMqttLink: the returned values are therefore indicative, which is fine
 * for the sole purpose of debug/diagnostics.
 */
class MqttManager {
public:
    /*! @brief Constructor.
     *  @param[in] txQueue FreeRTOS queue from which this task reads the JSON to publish.
     *  @param[in] rxQueue FreeRTOS queue on which this task writes the commands received via MQTT.
     *  @param[in] configManager Reference to the ConfigManager to read broker/port/credentials. */
    MqttManager(QueueHandle_t txQueue, QueueHandle_t rxQueue, ConfigManager& configManager);

    /*! @brief Static entry point for `xTaskCreatePinnedToCore`: forwards to the instance's taskRun().
     *  @param[in] pvParameters Pointer to the MqttManager the task runs on. */
    static void taskEntry(void* pvParameters);

    /*! @brief Initializes the client ID, topics, transport (TLS/plaintext) and
     *  PubSubClient parameters.
     *  @details Generates the MQTT client ID from the MAC (`SmartVase_HUB_<MAC>`),
     *  computes the `command/#` and `status` topics from `HUB_DEVICE_ID`, and
     *  sets the HiveMQ CA cert only if the configured port requires TLS (8883/8884).
     *  @note To be called once from setup(), BEFORE the creation of the
     *  TaskMqttLink task (taskEntry() does not call it, to avoid a double init). */
    void init();

    // --- State for the debug CLI ---
    /*! @brief @return true if a non-empty MQTT broker is configured in NVS.
     *  @note Read not synchronized with the MQTT task: indicative value, for debug/CLI only. */
    bool isConfigured() {
        const char* b = _configManager.getMqttBroker();
        return b != nullptr && strlen(b) > 0;
    }
    /*! @brief @return true if the PubSubClient client appears connected to the broker.
     *  @note Read not synchronized with the MQTT task: indicative value, for debug/CLI only. */
    bool isConnected() { return _mqttClient.connected(); }

private:
    QueueHandle_t _txQueue; /**< Queue to receive the JSON to publish (produced by MainLogic). */
    QueueHandle_t _rxQueue; /**< Queue to forward to MainLogic the commands received via MQTT. */

    ConfigManager& _configManager; /**< Reference to the configuration manager (broker, credentials). */

    // WiFi transport: plaintext for a broker on port 1883, TLS for 8883/8884.
    WiFiClient       _wifiClient;       /**< Plaintext transport, used if the configured port is 1883. */
    WiFiClientSecure _wifiClientSecure; /**< TLS transport (HiveMQ CA), used for ports 8883/8884. */
    PubSubClient     _mqttClient;       /**< MQTT client (PubSubClient library) on top of one of the two transports. */

    char _mqttBuffer[MQTT_BUFFER_SIZE]; /**< Internal PubSubClient buffer for the MQTT frames. */

    String _mqttClientId; /**< Unique MQTT client ID, derived from the STA Wi-Fi MAC. */
    String _topicCommand; /**< Command subscription topic, e.g. smartvase/HUB_123456/command/#. */
    String _topicStatus;  /**< Status/LWT topic, e.g. smartvase/HUB_123456/status. */

    /*! @brief Body of the FreeRTOS task `TaskMqttLink`: infinite loop that
     *  handles connection, keep-alive (PubSubClient::loop()) and message
     *  publishing/reception.
     *  @details If the broker is not configured, it drains and discards the TX
     *  queue without attempting any connection. If disconnected, it attempts
     *  reconnect() and meanwhile discards from the TX queue only the telemetry
     *  messages (they lose value if stale), while keeping alarm/log/ack for when
     *  it comes back online. */
    void taskRun();

    /*! @brief Attempts the (re)connection to the MQTT broker, NON-blocking.
     *  @details If TLS is required (port 8883/8884) and the system time is not
     *  valid yet (NTP not synchronized), it postpones the connect and forces a
     *  new SNTP attempt every 30 s instead of waiting for the SNTP library's
     *  default hourly retry. It then applies an exponential backoff
     *  (5->10->20->30 s) with jitter between the actual connect attempts.
     *  @return true if the connection (or reconnection) succeeded.
     *  @note "Non-blocking" here means it returns immediately if the backoff has
     *  not elapsed yet: the call cadence is set by the vTaskDelay in taskRun(). */
    bool reconnect();

    /*! @brief PubSubClient callback invoked for every message arriving on the subscribed topics.
     *  @param[in] topic MQTT topic of the received message.
     *  @param[in] payload Payload buffer (not NUL-terminated).
     *  @param[in] length Payload length in bytes.
     *  @note Must be static (PubSubClient library constraint): it uses
     *  `_instance` to reach the object state. Payloads >= sizeof(MqttCommand::payload)
     *  are discarded with an error log. */
    static void mqttCallback(char* topic, byte* payload, unsigned int length);

    static MqttManager* _instance; /**< Static pointer to the single instance, used by the static callback mqttCallback(). */
};

/*! @} */ // end of HubNetworking group

#endif // MQTTMANAGER_H
