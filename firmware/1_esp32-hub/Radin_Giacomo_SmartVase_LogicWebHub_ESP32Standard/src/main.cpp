/*
 * =================================================================
 * SmartVase - ESP32 Hub (Gateway)
 * Firmware Versione 1.0
 * =================================================================
 * Questo file contiene il punto di ingresso (setup) e
 * il loop principale.
 *
 * Architettura: FreeRTOS
 * Il setup inizializza i moduli e crea i Task.
 * Il loop gestisce solo attività a bassa priorità (es. CLI).
 * =================================================================
 */

#include <Arduino.h>
#include "ConfigManager.h"
#include "WifiManager.h"
#include "SerialManager.h"
#include "MQTTManager.h"
#include "MainLogic.h"

// --- Definizioni delle Code FreeRTOS ---
// Code per la comunicazione seriale (Mega <-> Hub)
QueueHandle_t serialRxQueue; // Messaggi Protobuf dal Mega all'Hub (MainLogic)
QueueHandle_t serialTxQueue; // Messaggi Protobuf dall'Hub (MainLogic) al Mega

// Code per la comunicazione MQTT (Hub <-> Broker)
QueueHandle_t mqttTxQueue;   // Messaggi JSON dall'Hub (MainLogic) al Broker MQTT
QueueHandle_t mqttRxQueue;   // Messaggi JSON dal Broker MQTT all'Hub (MainLogic)

// --- Istanziazione dei Moduli Globali ---
ConfigManager configManager;
WifiManager wifiManager(configManager);
SerialManager serialManager(serialRxQueue, serialTxQueue);
MqttManager mqttManager(mqttTxQueue, mqttRxQueue, configManager);
MainLogic mainLogic(serialRxQueue, serialTxQueue, mqttTxQueue, mqttRxQueue, configManager);

// =================================================================
// SETUP
// =================================================================
void setup() {
    // 1. Inizializza la seriale di Debug (USB)
    Serial.begin(115200);
    Serial.println("\n[SmartVase Hub] Avvio... v1.0");

    // 2. Inizializza il gestore della configurazione NVS
    if (!configManager.init()) {
        Serial.println("[CRITICAL] Impossibile inizializzare NVS. Riavvio.");
        delay(2000);
        ESP.restart();
    }
    configManager.loadConfig();

    // 3. Inizializza e avvia il Wi-Fi
    wifiManager.connect();
    Serial.println("[SETUP] Connessione Wi-Fi stabilita.");

    // 4. Creazione delle Code FreeRTOS
    serialRxQueue = xQueueCreate(10, sizeof(SerialMessage)); // Coda per messaggi dal Mega
    serialTxQueue = xQueueCreate(10, sizeof(SerialMessage)); // Coda per messaggi al Mega
    mqttTxQueue = xQueueCreate(10, sizeof(MqttMessage));     // Coda per messaggi MQTT in uscita
    mqttRxQueue = xQueueCreate(10, sizeof(MqttCommand));   // Coda per comandi MQTT in entrata

    if (serialRxQueue == NULL || serialTxQueue == NULL || mqttTxQueue == NULL || mqttRxQueue == NULL) {
        Serial.println("[CRITICAL] Impossibile creare le code FreeRTOS. Riavvio.");
        delay(2000);
        ESP.restart();
    }

    // 5. Inizializzazione dei Manager
    serialManager.init();
    mqttManager.init();
    mainLogic.init();

    // 6. Creazione dei Task FreeRTOS
    xTaskCreatePinnedToCore(
        SerialManager::taskEntry,   // Funzione del task (statica)
        "TaskSerialMega",           // Nome
        4096,                       // Stack size
        &serialManager,             // Parametri: puntatore all'istanza
        3,                          // Priorità (alta)
        NULL,                       // Handle
        1);                         // Core 1

    xTaskCreatePinnedToCore(
        MqttManager::taskEntry,     // Funzione del task (statica)
        "TaskMqttLink",             // Nome
        8192,                       // Stack size (aumentato per MQTT + SSL)
        &mqttManager,               // Parametri: puntatore all'istanza
        2,                          // Priorità (media)
        NULL,                       // Handle
        0);                         // Core 0

    xTaskCreatePinnedToCore(
        MainLogic::taskEntry,       // Funzione del task (statica)
        "TaskMainLogic",            // Nome
        8192,                       // Stack size (aumentato per JSON/Protobuf)
        &mainLogic,                 // Parametri: puntatore all'istanza
        1,                          // Priorità (bassa)
        NULL,                       // Handle
        0);                         // Core 0

    Serial.println("[SETUP] Setup completato. Avvio dei Task.");
}

// =================================================================
// LOOP (Task Principale Arduino)
// =================================================================
void loop() {
    // Il loop() è esso stesso un task (il task a priorità più bassa).
    // Lo usiamo per gestire la CLI di debug, un'attività non critica.
    
    // handleCLI(); // (da implementare)

    // Diamo un piccolo respiro al task
    vTaskDelay(pdMS_TO_TICKS(100));
}
