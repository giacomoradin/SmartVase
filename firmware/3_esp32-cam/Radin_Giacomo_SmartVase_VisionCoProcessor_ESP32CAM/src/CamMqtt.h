#ifndef CAM_MQTT_H
#define CAM_MQTT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "ConfigManager.h"
#include "VisionBotanist.h"

extern PubSubClient mqttClient;

void mqttInit();
void mqttLoop();
void mqttPublishVision(const String& imageUrl, size_t bytes, uint32_t crc, uint32_t capMs, const AnalysisResult& analysis);

#endif // CAM_MQTT_H
