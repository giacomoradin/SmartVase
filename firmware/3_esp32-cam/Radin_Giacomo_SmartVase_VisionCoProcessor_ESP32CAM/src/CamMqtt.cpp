#include "CamMqtt.h"
#include <WiFi.h>
#include "hivemq_ca_cert.h"
#include <ArduinoJson.h>

static WiFiClientSecure mqttSslClient;
PubSubClient mqttClient(mqttSslClient);

static unsigned long lastMqttRetryMs = 0;
#define MQTT_RETRY_INTERVAL_MS 5000UL

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[CAM MQTT] Message arrived on topic: %s\n", topic);
    // Topic structure: smartvase/CAM_123456/command/capture_requested
    if (strstr(topic, "/command/capture_requested") != nullptr || strstr(topic, "capture_requested") != nullptr) {
        Serial.println("[CAM MQTT] Capture requested via MQTT command!");
        extern bool captureRequested;
        captureRequested = true;
    }
}

void mqttInit() {
    if (cfg.mqtt_broker.length() == 0 || cfg.mqtt_port == 0) {
        Serial.println("[CAM MQTT] Broker unconfigured.");
        return;
    }
    if (cfg.mqtt_port == 8883 || cfg.mqtt_port == 8884) {
        mqttSslClient.setCACert(SMARTVASE_HIVEMQ_CA_CERT);
    }
    mqttClient.setServer(cfg.mqtt_broker.c_str(), cfg.mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);
    mqttClient.setSocketTimeout(10);
    Serial.printf("[CAM MQTT] Initialized for broker: %s:%d\n", cfg.mqtt_broker.c_str(), cfg.mqtt_port);
}

static void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (cfg.mqtt_broker.length() == 0) return;

    // If TLS is used, ensure NTP is valid
    if ((cfg.mqtt_port == 8883 || cfg.mqtt_port == 8884) && time(nullptr) < NTP_VALID_EPOCH) {
        return;
    }

    if (millis() - lastMqttRetryMs < MQTT_RETRY_INTERVAL_MS && lastMqttRetryMs != 0) {
        return;
    }
    lastMqttRetryMs = millis();

    String clientId = "SmartVase_" + String(deviceId) + "_" + String(millis());
    Serial.printf("[CAM MQTT] Attempting connection as %s...\n", clientId.c_str());

    bool connected = false;
    if (cfg.mqtt_user.length() > 0) {
        connected = mqttClient.connect(clientId.c_str(), cfg.mqtt_user.c_str(), cfg.mqtt_password.c_str());
    } else {
        connected = mqttClient.connect(clientId.c_str());
    }

    if (connected) {
        Serial.println("[CAM MQTT] Connected to broker!");
        String cmdTopic = "smartvase/" + String(deviceId) + "/command/#";
        mqttClient.subscribe(cmdTopic.c_str());
        Serial.printf("[CAM MQTT] Subscribed to %s\n", cmdTopic.c_str());
    } else {
        Serial.printf("[CAM MQTT] Connect failed, rc=%d\n", mqttClient.state());
    }
}

void mqttLoop() {
    if (cfg.mqtt_broker.length() == 0) return;
    if (!mqttClient.connected()) {
        mqttReconnect();
    } else {
        mqttClient.loop();
    }
}

void mqttPublishVision(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs, const AnalysisResult& analysis) {
    if (!mqttClient.connected()) {
        Serial.println("[CAM MQTT] Cannot publish vision notification: offline.");
        return;
    }

    time_t nowEpoch = time(nullptr);
    String topic = "smartvase/" + String(deviceId) + "/vision/latest";

    StaticJsonDocument<512> doc;
    doc["timestamp_utc"]   = (nowEpoch > NTP_VALID_EPOCH) ? nowEpoch : 0;
    doc["image_url"]       = imageUrl;
    doc["resolution"]      = psramFound() ? "800x600" : "640x480";
    doc["size_bytes"]      = bytes;
    doc["crc32"]           = crc;
    doc["capture_time_ms"] = capMs;
    doc["plant_healthy"]   = analysis.plant_healthy;
    doc["status_message"]  = analysis.status_message;
    doc["foliage_coverage"]= String(analysis.foliage_coverage * 100.0f, 1) + "%";
    doc["green_ratio"]     = String(analysis.green_ratio * 100.0f, 1) + "%";
    doc["brown_ratio"]     = String(analysis.brown_ratio * 100.0f, 1) + "%";
    doc["device_id"]       = deviceId;

    char buffer[512];
    size_t len = serializeJson(doc, buffer, sizeof(buffer));

    Serial.printf("[CAM MQTT] Publishing vision notification to topic %s (%u bytes)...\n", topic.c_str(), (unsigned)len);
    bool pubOk = mqttClient.publish(topic.c_str(), (uint8_t*)buffer, len, true); // retain = true
    if (pubOk) {
        Serial.println("[CAM MQTT] Vision notification published successfully.");
    } else {
        Serial.println("[CAM MQTT] Failed to publish vision notification.");
    }
}
