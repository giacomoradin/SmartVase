/*! @file main.cpp
 *  @ingroup HubCore
 *  @brief Hub firmware entry point: bootstraps NVS/Wi-Fi/FreeRTOS queues,
 *  creates the three pinned tasks (TaskSerialMega, TaskMqttLink,
 *  TaskMainLogic) and the Arduino loop dedicated to CLI/provisioning/OTA.
 *  @details setup() creates the FreeRTOS queues BEFORE instantiating the
 *  modules that use them (SerialManager, MqttManager, MainLogic): an earlier
 *  attempt instantiated them as global objects, whose constructors copied
 *  the queue handles while they were still NULL (static initialization),
 *  causing the tasks to start on non-existent queues. loop() only handles
 *  non time-critical, lowest-priority activity (serial CLI, captive portal,
 *  OTA): the real logic runs in the three FreeRTOS tasks created by setup().
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
#include "secrets.h"     // SV_OTA_PASS (gitignored)

// --- FreeRTOS Queues ---
// Serial communication (Mega <-> Hub)
QueueHandle_t serialRxQueue; /**< Protobuf messages from the Mega to the Hub (consumed by MainLogic). */
QueueHandle_t serialTxQueue; /**< Protobuf messages from the Hub (MainLogic/HubCli) to the Mega. */

// MQTT communication (Hub <-> Broker)
QueueHandle_t mqttTxQueue;   /**< JSON messages from the Hub (MainLogic) to the MQTT broker. */
QueueHandle_t mqttRxQueue;   /**< JSON messages from the MQTT broker to the Hub (MainLogic). */

// --- Modules ---
// No dependency on the queues: global instances.
ConfigManager configManager;          /**< Persistent configuration (NVS). */
WifiManager   wifiManager(configManager); /**< Wi-Fi STA connection + provisioning AP. */
HubCli        hubCli;                 /**< Serial debug/provisioning CLI. */

// Dependent on the queues: created in setup() after xQueueCreate.
SerialManager* serialManager = nullptr; /**< Body of the TaskSerialMega task; allocated in setup(). */
MqttManager*   mqttManager   = nullptr; /**< Body of the TaskMqttLink task; allocated in setup(). */
MainLogic*     mainLogic     = nullptr; /**< Body of the TaskMainLogic task; allocated in setup(). */

/*! @brief Arduino entry point: initializes NVS/Wi-Fi, creates the four
 *  FreeRTOS queues and the modules that use them, then starts the three
 *  pinned tasks (TaskSerialMega on Core 1 prio 3, TaskMqttLink on Core 0
 *  prio 2, TaskMainLogic on Core 1 prio 1).
 *  @details Critical ordering: the queues are created BEFORE instantiating
 *  SerialManager/MqttManager/MainLogic, which receive them by pointer in
 *  the constructor (see the module comment above). On NVS or xQueueCreate()
 *  failure, performs a restart (ESP.restart()) after a short diagnostic
 *  delay: there is no degraded path without working queues. */
void setup() {
    // 1. Debug/CLI Serial (USB)
    pinMode(3, INPUT_PULLUP); // Prevents RXD0 from floating when USB is not connected
    Serial.begin(115200);
    Serial.println("\n[SmartVase Hub] Avvio... v1.3");

    // 2. NVS Configuration
    if (!configManager.init()) {
        Serial.println("[CRITICAL] Impossibile inizializzare NVS. Riavvio.");
        delay(2000);
        ESP.restart();
    }
    configManager.loadConfig();

    // 3. FreeRTOS queues (before the modules that use them)
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

    // 4. Modules dependent on the queues
    serialManager = new SerialManager(serialRxQueue, serialTxQueue);
    mqttManager   = new MqttManager(mqttTxQueue, mqttRxQueue, configManager);
    mainLogic     = new MainLogic(serialRxQueue, serialTxQueue,
                                  mqttTxQueue, mqttRxQueue, configManager);

    // 5. Wi-Fi: attempt with timeout; if it fails, continue offline
    //    (provisioning from the serial CLI, automatic retry in the background).
    wifiManager.connect();

    // 6. Manager initialization
    serialManager->init();
    mqttManager->init();
    mainLogic->init();
    hubCli.begin(&configManager, &wifiManager, mqttManager, mainLogic, serialTxQueue);

    // 7. FreeRTOS Tasks
    xTaskCreatePinnedToCore(
        SerialManager::taskEntry,   // Task function (static)
        "TaskSerialMega",           // Name
        4096,                       // Stack size
        serialManager,              // Parameters: pointer to the instance
        3,                          // Priority (high)
        NULL,                       // Handle
        1);                         // Core 1

    xTaskCreatePinnedToCore(
        MqttManager::taskEntry,     // Task function (static)
        "TaskMqttLink",             // Name
        8192,                       // Stack size (MQTT + SSL)
        mqttManager,                // Parameters: pointer to the instance
        2,                          // Priority (medium)
        NULL,                       // Handle
        0);                         // Core 0

    xTaskCreatePinnedToCore(
        MainLogic::taskEntry,       // Task function (static)
        "TaskMainLogic",            // Name
        8192,                       // Stack size (JSON/Protobuf)
        mainLogic,                  // Parameters: pointer to the instance
        1,                          // Priority (low)
        NULL,                       // Handle
        1);                         // Core 1 (isolated from TLS spikes on Core 0)

    Serial.println("[SETUP] Setup completato. Avvio dei Task.");
}

/*! @brief Main Arduino loop: only handles non time-critical activity at the
 *  lowest priority (the core logic runs in the FreeRTOS tasks started by setup()).
 *  @details On each pass: drains the serial CLI (hubCli.tick()), advances
 *  the Captive Portal if the provisioning AP is active (no-op otherwise),
 *  and starts/handles ArduinoOTA once the Wi-Fi STA is connected.
 *  @note OTA startup (`ArduinoOTA.begin()`) happens exactly once, guarded by
 *  the local `otaStarted` flag; from that point on `ArduinoOTA.handle()` is
 *  called on every iteration regardless of the connection state. */
void loop() {
    // Debug/provisioning CLI: non-critical activity at the lowest priority.
    hubCli.tick();

    // No-op if the provisioning AP is not active.
    wifiManager.handleProvisioning();

    // OTA: started once when Wi-Fi comes up; then handle() on every pass.
    // Additive and passive (does not interfere with normal operation). Real
    // update validation must be done on the bench (requires network + tooling).
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
