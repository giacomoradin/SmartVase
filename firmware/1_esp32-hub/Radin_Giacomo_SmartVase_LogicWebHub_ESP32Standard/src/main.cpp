/*
 * =================================================================
 * SmartVase - ESP32 Hub (Gateway)
 * Firmware Versione 1.2 — 2026-06-11 (hardening pre-bring-up)
 * =================================================================
 * Punto di ingresso: setup() crea code e moduli, poi i task FreeRTOS.
 * loop() gestisce solo la CLI seriale di debug/provisioning.
 *
 * Nota d'ordine: i manager sono allocati in setup() DOPO la creazione
 * delle code FreeRTOS. La versione precedente li istanziava come oggetti
 * globali: i costruttori copiavano gli handle delle code quando erano
 * ancora NULL (static init) e i task partivano su code inesistenti.
 * =================================================================
 */

#include <Arduino.h>
#include "ConfigManager.h"
#include "WifiManager.h"
#include "SerialManager.h"
#include "MQTTManager.h"
#include "MainLogic.h"
#include "HubCli.h"

// --- Code FreeRTOS ---
// Comunicazione seriale (Mega <-> Hub)
QueueHandle_t serialRxQueue; // Messaggi Protobuf dal Mega all'Hub (MainLogic)
QueueHandle_t serialTxQueue; // Messaggi Protobuf dall'Hub (MainLogic/CLI) al Mega

// Comunicazione MQTT (Hub <-> Broker)
QueueHandle_t mqttTxQueue;   // Messaggi JSON dall'Hub (MainLogic) al Broker MQTT
QueueHandle_t mqttRxQueue;   // Messaggi JSON dal Broker MQTT all'Hub (MainLogic)

// --- Moduli ---
// Senza dipendenze dalle code: istanze globali.
ConfigManager configManager;
WifiManager   wifiManager(configManager);
HubCli        hubCli;

// Dipendenti dalle code: creati in setup() dopo xQueueCreate.
SerialManager* serialManager = nullptr;
MqttManager*   mqttManager   = nullptr;
MainLogic*     mainLogic     = nullptr;

// =================================================================
// SETUP
// =================================================================
void setup() {
    // 1. Seriale di Debug/CLI (USB)
    Serial.begin(115200);
    Serial.println("\n[SmartVase Hub] Avvio... v1.2");

    // 2. Configurazione NVS
    if (!configManager.init()) {
        Serial.println("[CRITICAL] Impossibile inizializzare NVS. Riavvio.");
        delay(2000);
        ESP.restart();
    }
    configManager.loadConfig();

    // 3. Code FreeRTOS (prima dei moduli che le usano)
    serialRxQueue = xQueueCreate(10, sizeof(SerialMessage));
    serialTxQueue = xQueueCreate(10, sizeof(SerialMessage));
    mqttTxQueue   = xQueueCreate(10, sizeof(MqttMessage));
    mqttRxQueue   = xQueueCreate(10, sizeof(MqttCommand));

    if (serialRxQueue == NULL || serialTxQueue == NULL ||
        mqttTxQueue == NULL || mqttRxQueue == NULL) {
        Serial.println("[CRITICAL] Impossibile creare le code FreeRTOS. Riavvio.");
        delay(2000);
        ESP.restart();
    }

    // 4. Moduli dipendenti dalle code
    serialManager = new SerialManager(serialRxQueue, serialTxQueue);
    mqttManager   = new MqttManager(mqttTxQueue, mqttRxQueue, configManager);
    mainLogic     = new MainLogic(serialRxQueue, serialTxQueue,
                                  mqttTxQueue, mqttRxQueue, configManager);

    // 5. Wi-Fi: tentativo con timeout; se fallisce si continua offline
    //    (provisioning dalla CLI seriale, retry automatico in background).
    wifiManager.connect();

    // 6. Inizializzazione dei Manager
    serialManager->init();
    mqttManager->init();
    mainLogic->init();
    hubCli.begin(&configManager, &wifiManager, mqttManager, mainLogic, serialTxQueue);

    // 7. Task FreeRTOS
    xTaskCreatePinnedToCore(
        SerialManager::taskEntry,   // Funzione del task (statica)
        "TaskSerialMega",           // Nome
        4096,                       // Stack size
        serialManager,              // Parametri: puntatore all'istanza
        3,                          // Priorità (alta)
        NULL,                       // Handle
        1);                         // Core 1

    xTaskCreatePinnedToCore(
        MqttManager::taskEntry,     // Funzione del task (statica)
        "TaskMqttLink",             // Nome
        8192,                       // Stack size (MQTT + SSL)
        mqttManager,                // Parametri: puntatore all'istanza
        2,                          // Priorità (media)
        NULL,                       // Handle
        0);                         // Core 0

    xTaskCreatePinnedToCore(
        MainLogic::taskEntry,       // Funzione del task (statica)
        "TaskMainLogic",            // Nome
        8192,                       // Stack size (JSON/Protobuf)
        mainLogic,                  // Parametri: puntatore all'istanza
        1,                          // Priorità (bassa)
        NULL,                       // Handle
        0);                         // Core 0

    Serial.println("[SETUP] Setup completato. Avvio dei Task.");
}

// =================================================================
// LOOP (Task Principale Arduino)
// =================================================================
void loop() {
    // CLI di debug/provisioning: attività non critica a priorità minima.
    hubCli.tick();

    // No-op se l'AP di provisioning non è attivo.
    wifiManager.handleProvisioning();

    vTaskDelay(pdMS_TO_TICKS(20));
}
