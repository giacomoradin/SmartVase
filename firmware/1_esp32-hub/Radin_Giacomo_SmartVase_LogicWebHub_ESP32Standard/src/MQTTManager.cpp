/*! @file MQTTManager.cpp
 *  @ingroup HubNetworking
 *  @brief Implementation of MqttManager: TLS/plaintext transport init,
 *  connection/publish loop, callback for incoming messages.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include "MqttManager.h"
#include "esp_log.h"
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_random.h"
#include "esp_sntp.h"
#include "hivemq_ca_cert.h"

static const char *TAG = "MqttManager";

/*! @brief Minimum epoch below which the system time is considered NOT valid
 *         (clock not yet synchronized via NTP): the HiveMQ TLS certificate
 *         validity check would fail, so we wait for NTP before connecting. */
#define NTP_VALID_EPOCH 1700000000UL

// HiveMQ Cloud cert (ISRG Root X1). Defined in include/hivemq_ca_cert.h
// (shared with the CAM via infra/hivemq_ca_cert.h).
static const char* hivemq_ca_cert = SMARTVASE_HIVEMQ_CA_CERT;

// Initialize the static instance pointer
MqttManager* MqttManager::_instance = nullptr;

// --- MqttManager Class Method Implementation ---

MqttManager::MqttManager(QueueHandle_t txQueue, QueueHandle_t rxQueue, ConfigManager& configManager)
    : _txQueue(txQueue),
      _rxQueue(rxQueue),
      _configManager(configManager),
      _mqttClient(_wifiClientSecure) // Passa il client sicuro al costruttore di PubSubClient
{
    _instance = this; // Store the instance pointer for the static callback
}

void MqttManager::init() {
    ESP_LOGI(TAG, "Initializing MQTT Manager...");

    // Generate a unique Client ID based on the MAC Address.
    // esp_read_mac reads from the eFuse and works even with the radio off.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    _mqttClientId = "SmartVase_HUB_";
    char macSuffix[13]; // 6 byte * 2 char/byte + 1 null terminator
    snprintf(macSuffix, sizeof(macSuffix), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _mqttClientId += macSuffix;
    ESP_LOGI(TAG, "MQTT Client ID: %s", _mqttClientId.c_str());

    // STATIC device ID from the single source HUB_DEVICE_ID (ConfigManager.h),
    // consistent with MainLogic. The MAC is only used for the MQTT Client ID above.
    _topicCommand = String("smartvase/") + HUB_DEVICE_ID + "/command/#"; // Subscribes to all sub-topics
    _topicStatus  = String("smartvase/") + HUB_DEVICE_ID + "/status";
    ESP_LOGI(TAG, "Command Topic: %s", _topicCommand.c_str());
    ESP_LOGI(TAG, "Status Topic (LWT): %s", _topicStatus.c_str());


    // Transport chosen based on the port: TLS (8883/8884, with CA cert + NTP)
    // or plaintext (1883, local broker: no TLS, no NTP required).
    const char* broker = _configManager.getMqttBroker();
    uint16_t port = _configManager.getMqttPort();
    if (port == 8883 || port == 8884) {
        _wifiClientSecure.setCACert(hivemq_ca_cert);
        _mqttClient.setClient(_wifiClientSecure);
        ESP_LOGI(TAG, "MQTT transport: TLS (porta %u)", port);
    } else {
        _mqttClient.setClient(_wifiClient);
        ESP_LOGW(TAG, "MQTT transport: IN CHIARO (porta %u) - nessuna TLS/NTP", port);
    }

    if (broker && strlen(broker) > 0 && port > 0) {
        _mqttClient.setServer(broker, port);
        ESP_LOGI(TAG, "MQTT Broker set to: %s:%d", broker, port);
    } else {
        ESP_LOGE(TAG, "MQTT Broker configuration missing or invalid!");
    }

    // Set the callback function for incoming messages
    _mqttClient.setCallback(MqttManager::mqttCallback);

    // Set PubSubClient's internal buffer size (if needed)
    _mqttClient.setBufferSize(MQTT_BUFFER_SIZE);

    // Bounds the wait for CONNACK/reads: prevents a dead broker from keeping
    // the task hanging for a long time (resilience; PubSubClient default = 15 s).
    _mqttClient.setSocketTimeout(10);
}

// Static entry point function for the FreeRTOS Task
void MqttManager::taskEntry(void* pvParameters) {
    MqttManager* instance = static_cast<MqttManager*>(pvParameters);
    // init() is already called by setup() before the task is created:
    // it is not repeated here, to avoid double initialization.
    instance->taskRun(); // Enter the task's main loop
}

// Main task function (infinite loop)
void MqttManager::taskRun() {
    ESP_LOGI(TAG, "MqttManager Task Started.");
    MqttMessage msgToPublish;
    bool warnedNotConfigured = false;

    while (true) {
        // 0. Broker not configured: no connection attempt.
        //    The TX queue is drained anyway, to avoid accumulating messages.
        if (!isConfigured()) {
            if (!warnedNotConfigured) {
                warnedNotConfigured = true;
                ESP_LOGW(TAG, "Broker MQTT non configurato: pubblicazione disattivata.");
                Serial.println(F("[MQTT] Non configurato. Dalla CLI: set mqtt_broker <host>, set mqtt_user/mqtt_pass, save, reboot"));
            }
            while (xQueueReceive(_txQueue, &msgToPublish, 0) == pdPASS) { /* discard */ }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 1. Make sure the MQTT client is connected
        if (!_mqttClient.connected()) {
            // Attempt reconnection (with internal backoff logic).
            // Meanwhile the TX queue is drained: telemetry is periodic,
            // accumulating it offline has no value.
            reconnect();
            // Offline: discard ONLY telemetry (loses value if stale) but
            // keep alarm/ack/log queued to send them on reconnection.
            // Bounded: a single pass over the messages currently present.
            UBaseType_t pending = uxQueueMessagesWaiting(_txQueue);
            for (UBaseType_t i = 0; i < pending; ++i) {
                if (xQueueReceive(_txQueue, &msgToPublish, 0) != pdPASS) break;
                if (strstr(msgToPublish.topic, "/telemetry") != nullptr) continue; // discard
                if (xQueueSend(_txQueue, &msgToPublish, 0) != pdPASS) break;        // put back in the queue
            }
        } else {
            // 2. If connected, process incoming MQTT messages and keep the connection alive
            _mqttClient.loop();

            // 3. Check whether there are JSON messages to publish from the _txQueue
            //    Check without blocking (timeout 0)
            if (xQueueReceive(_txQueue, &msgToPublish, 0) == pdPASS) {
                ESP_LOGD(TAG, "Publishing message from TX queue to topic: %s", msgToPublish.topic);
                //ESP_LOGV(TAG, "Payload: %s", msgToPublish.payload); // Log the payload only at Verbose level

                // Publish the message
                // Parameters: topic, payload (byte array), payload length, retain (false)
                if (!_mqttClient.publish(msgToPublish.topic, (uint8_t*)msgToPublish.payload, strlen(msgToPublish.payload), false)) {
                    ESP_LOGE(TAG, "MQTT Publish failed! Topic: %s", msgToPublish.topic);
                    // Here you could requeue the message or discard it
                } else {
                     ESP_LOGD(TAG, "Publish successful.");
                }
            }
        } // End else (connected)

        // 4. Task pause to yield the CPU
        //    The frequency depends on how reactive the MQTT client needs to be
        //    and how often we expect messages to publish.
        vTaskDelay(pdMS_TO_TICKS(100)); // Check/process every 100ms
    }
}

// Attempts to reconnect to the MQTT broker. NON-blocking: returns immediately
// if the backoff has not expired yet (the pacing comes from taskRun's
// vTaskDelay), so the task stays reactive instead of sleeping 5 s per attempt.
bool MqttManager::reconnect() {
    // No Wi-Fi: no attempt, no blocking delay.
    if (!WiFi.isConnected()) return false;

    // NTP is needed ONLY for TLS (cert date validation): with a plaintext
    // broker (1883) it is skipped entirely, so an offline local network can
    // still connect. With the clock at 1970 the HiveMQ cert shows as "not yet
    // valid": SNTP retries by default ~every hour, so if it stays invalid I
    // force a new attempt every 30 s by restarting SNTP.
    uint16_t mqttPort = _configManager.getMqttPort();
    bool needsTLS = (mqttPort == 8883 || mqttPort == 8884);
    if (needsTLS && time(nullptr) < NTP_VALID_EPOCH) {
        static uint32_t lastNtpKickMs = 0;
        uint32_t nowMs = millis();
        if (lastNtpKickMs == 0) {
            configTime(0, 0, "pool.ntp.org", "time.google.com"); // avvia SNTP
            lastNtpKickMs = nowMs;
            ESP_LOGW(TAG, "Ora non valida: NTP avviato, rimando il connect MQTT.");
        } else if (nowMs - lastNtpKickMs > 30000UL) {
            sntp_stop();
            configTime(0, 0, "pool.ntp.org", "time.google.com"); // forza nuovo tentativo
            lastNtpKickMs = nowMs;
            ESP_LOGW(TAG, "NTP ancora non sincronizzato: nuovo tentativo.");
        }
        return false;
    }

    // Backoff con tetto 30 s + jitter, gestito a timer (no delay bloccante).
    static uint32_t lastAttemptMs = 0;
    static uint32_t backoffMs     = 0;
    uint32_t now = millis();
    if (lastAttemptMs != 0 && (now - lastAttemptMs) < backoffMs) return false;
    lastAttemptMs = now;

    ESP_LOGI(TAG, "Attempting MQTT connection...");
    const char* user     = _configManager.getMqttUser();
    const char* password = _configManager.getMqttPassword();

    // LWT: alla disconnessione anomala il broker pubblica "offline" su /status.
    if (_mqttClient.connect(_mqttClientId.c_str(), user, password,
                            _topicStatus.c_str(), 1, true, "offline")) {
        ESP_LOGI(TAG, "MQTT Connected!");
        backoffMs = 0; // reset backoff a connessione riuscita

        String onlinePayload = "online";
        _mqttClient.publish(_topicStatus.c_str(), (uint8_t*)onlinePayload.c_str(),
                            onlinePayload.length(), true); // retain

        if (_mqttClient.subscribe(_topicCommand.c_str())) {
            ESP_LOGI(TAG, "Subscribed to command topic: %s", _topicCommand.c_str());
        } else {
            ESP_LOGE(TAG, "Failed to subscribe to command topic!");
        }
        return true;
    }

    // Backoff esponenziale (5→10→20→30 s) + jitter per evitare polling sincrono.
    backoffMs = (backoffMs == 0) ? 5000 : (backoffMs < 30000 ? backoffMs * 2 : 30000);
    backoffMs += (esp_random() % 1000);
    ESP_LOGW(TAG, "MQTT connect failed rc=%d, retry tra ~%lu ms",
             _mqttClient.state(), (unsigned long)backoffMs);
    return false;
}

// Callback for incoming MQTT messages (MUST be static)
void MqttManager::mqttCallback(char* topic, byte* payload, unsigned int length) {
    ESP_LOGI(TAG, "Message arrived [%s]", topic);

    // Check whether the instance and the queue are valid
    if (!_instance || !_instance->_rxQueue) {
        ESP_LOGE(TAG, "Callback invoked without a valid instance or RX queue!");
        return;
    }

    // Controlla che il payload non sia troppo grande per la nostra struct
    if (length >= sizeof(MqttCommand::payload)) {
        ESP_LOGE(TAG, "Incoming MQTT message payload too large (%d bytes)! Max: %d. Discarding.", length, sizeof(MqttCommand::payload) - 1);
        return;
    }

    // Crea il comando da inviare alla coda
    MqttCommand cmd;
    cmd.timestamp = millis(); // Usa millis() o un'altra fonte di tempo

    // Copia il topic e il payload nel comando
    strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
    cmd.topic[sizeof(cmd.topic) - 1] = '\0'; // Assicura terminazione nulla

    memcpy(cmd.payload, payload, length);
    cmd.payload[length] = '\0'; // Assicura terminazione nulla

    ESP_LOGD(TAG, "Payload: %s", cmd.payload);

    // Invia il comando alla coda di MainLogic per il processamento
    // Usa un timeout breve per non bloccare il callback MQTT a lungo
    if (xQueueSend(_instance->_rxQueue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to queue incoming MQTT command. RX Queue may be full.");
    }
}
