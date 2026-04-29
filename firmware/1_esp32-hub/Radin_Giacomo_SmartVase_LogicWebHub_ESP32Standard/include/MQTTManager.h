#ifndef MQTTMANAGER_H
#define MQTTMANAGER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <WiFiClientSecure.h> // Per connessione TLS
#include <PubSubClient.h>     // Libreria MQTT

// Include le definizioni necessarie
#include "ConfigManager.h" // Per accedere alla configurazione MQTT
#include "MainLogic.h"     // Per la struct MqttMessage (definita lì)

// Dimensione buffer MQTT (per messaggi in ingresso/uscita)
// Deve essere >= MAX_JSON_MESSAGE_LENGTH + overhead topic
#define MQTT_BUFFER_SIZE (MAX_JSON_MESSAGE_LENGTH + 128)

class MqttManager {
public:
    // Costruttore: Riceve la coda TX e il ConfigManager
    MqttManager(QueueHandle_t txQueue, QueueHandle_t rxQueue, ConfigManager& configManager);

    // Funzione statica entry point per il Task FreeRTOS
    static void taskEntry(void* pvParameters);

    // Inizializza il client MQTT e configura la connessione sicura
    void init();

private:
    // Code FreeRTOS
    QueueHandle_t _txQueue; // Coda per inviare JSON a questo task
    QueueHandle_t _rxQueue; // Coda per inviare comandi ricevuti a MainLogic

    // Riferimento al ConfigManager
    ConfigManager& _configManager;

    // Client WiFi sicuro (per TLS) e client MQTT
    WiFiClientSecure _wifiClientSecure;
    PubSubClient _mqttClient;

    // Buffer per messaggi MQTT
    char _mqttBuffer[MQTT_BUFFER_SIZE];

    // Identificativo univoco del client MQTT (basato sul MAC)
    String _mqttClientId;
    // Topic specifici per questo device
    String _topicCommand; // Es. smartvase/HUB_A1B2C3/command/#
    String _topicStatus;  // Es. smartvase/HUB_A1B2C3/status (per LWT)

    // Funzione principale del Task (loop infinito)
    void taskRun();

    // Tenta la riconnessione al broker MQTT
    bool reconnect();

    // Callback per i messaggi MQTT in arrivo (deve essere statica o globale)
    static void mqttCallback(char* topic, byte* payload, unsigned int length);

    // Puntatore statico all'istanza per l'uso nel callback statico
    static MqttManager* _instance;
};

#endif // MQTTMANAGER_H
