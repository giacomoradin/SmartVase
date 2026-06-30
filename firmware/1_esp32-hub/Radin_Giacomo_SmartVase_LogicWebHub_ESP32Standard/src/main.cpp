/*! @file main.cpp
 *  @ingroup HubCore
 *  @brief Punto di ingresso del firmware Hub: bootstrap di NVS/Wi-Fi/code
 *  FreeRTOS, creazione dei tre task pinnati (TaskSerialMega, TaskMqttLink,
 *  TaskMainLogic) e loop Arduino dedicato a CLI/provisioning/OTA.
 *  @details setup() crea le code FreeRTOS PRIMA di istanziare i moduli che le
 *  usano (SerialManager, MqttManager, MainLogic): un tentativo precedente li
 *  istanziava come oggetti globali, i cui costruttori copiavano gli handle
 *  delle code quando erano ancora NULL (inizializzazione statica), facendo
 *  partire i task su code inesistenti. loop() gestisce solo attivita' non
 *  time-critical a priorita' minima (CLI seriale, captive portal, OTA): la
 *  logica vera gira nei tre task FreeRTOS creati da setup().
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "ConfigManager.h"
#include "WifiManager.h"
#include "SerialManager.h"
#include "MQTTManager.h"
#include "MainLogic.h"
#include "HubCli.h"
#include "secrets.h"     // SV_OTA_PASS (gitignorato)

// --- Code FreeRTOS ---
// Comunicazione seriale (Mega <-> Hub)
QueueHandle_t serialRxQueue; /**< Messaggi Protobuf dal Mega all'Hub (consumata da MainLogic). */
QueueHandle_t serialTxQueue; /**< Messaggi Protobuf dall'Hub (MainLogic/HubCli) al Mega. */

// Comunicazione MQTT (Hub <-> Broker)
QueueHandle_t mqttTxQueue;   /**< Messaggi JSON dall'Hub (MainLogic) al broker MQTT. */
QueueHandle_t mqttRxQueue;   /**< Messaggi JSON dal broker MQTT all'Hub (MainLogic). */

// --- Moduli ---
// Senza dipendenze dalle code: istanze globali.
ConfigManager configManager;          /**< Configurazione persistente (NVS). */
WifiManager   wifiManager(configManager); /**< Connessione Wi-Fi STA + provisioning AP. */
HubCli        hubCli;                 /**< CLI seriale di debug/provisioning. */

// Dipendenti dalle code: creati in setup() dopo xQueueCreate.
SerialManager* serialManager = nullptr; /**< Corpo del task TaskSerialMega; allocato in setup(). */
MqttManager*   mqttManager   = nullptr; /**< Corpo del task TaskMqttLink; allocato in setup(). */
MainLogic*     mainLogic     = nullptr; /**< Corpo del task TaskMainLogic; allocato in setup(). */

/*! @brief Entry point Arduino: inizializza NVS/Wi-Fi, crea le quattro code
 *  FreeRTOS e i moduli che le usano, poi avvia i tre task pinnati
 *  (TaskSerialMega su Core 1 prio 3, TaskMqttLink su Core 0 prio 2,
 *  TaskMainLogic su Core 1 prio 1).
 *  @details Ordine critico: le code vengono create PRIMA di istanziare
 *  SerialManager/MqttManager/MainLogic, che le ricevono per puntatore nel
 *  costruttore (vedi commento di modulo sopra). Su fallimento di NVS o delle
 *  xQueueCreate() esegue un riavvio (ESP.restart()) dopo un breve delay
 *  diagnostico: non c'e' un percorso degradato senza code funzionanti. */
void setup() {
    // 1. Seriale di Debug/CLI (USB)
    pinMode(3, INPUT_PULLUP); // Evita che RXD0 fluttui quando l'USB non e' connesso
    Serial.begin(115200);
    Serial.println("\n[SmartVase Hub] Avvio... v1.3");

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
        1);                         // Core 1 (isolato dai picchi TLS su Core 0)

    Serial.println("[SETUP] Setup completato. Avvio dei Task.");
}

/*! @brief Loop Arduino principale: gestisce solo attivita' non time-critical
 *  a priorita' minima (la logica core gira nei task FreeRTOS avviati da setup()).
 *  @details Ad ogni giro: drena la CLI seriale (hubCli.tick()), fa avanzare il
 *  Captive Portal se l'AP di provisioning e' attivo (no-op altrimenti), e
 *  avvia/gestisce ArduinoOTA una volta che il Wi-Fi STA risulta connesso.
 *  @note L'avvio di OTA (`ArduinoOTA.begin()`) avviene una sola volta tramite
 *  il flag locale `otaStarted`; da quel momento `ArduinoOTA.handle()` viene
 *  chiamato ad ogni iterazione indipendentemente dallo stato della connessione. */
void loop() {
    // CLI di debug/provisioning: attività non critica a priorità minima.
    hubCli.tick();

    // No-op se l'AP di provisioning non è attivo.
    wifiManager.handleProvisioning();

    // OTA: avvio una volta quando il Wi-Fi e' su; poi handle() ad ogni giro.
    // Additivo e passivo (non interferisce col funzionamento normale). La
    // validazione dell'update vero va fatta a banco (richiede rete + tool).
    static bool otaStarted = false;
    if (!otaStarted && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("smartvase-hub");
        ArduinoOTA.setPassword(SV_OTA_PASS);
        ArduinoOTA.begin();
        otaStarted = true;
        Serial.println("[OTA] pronto: hostname 'smartvase-hub'");
    }
    if (otaStarted) ArduinoOTA.handle();

    vTaskDelay(pdMS_TO_TICKS(20));
}
