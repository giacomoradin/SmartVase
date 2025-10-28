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
// Includeremo qui altri moduli (MqttManager, SerialMega, etc.)

// --- Istanziazione dei Moduli Globali ---
ConfigManager configManager;
WifiManager wifiManager(configManager); // Il WifiManager dipende dal ConfigManager

// --- Prototipi dei Task (da implementare) ---
void taskSerialMega(void *pvParameters);
void taskMqttLink(void *pvParameters);
void taskMainLogic(void *pvParameters);

// =================================================================
// SETUP
// =================================================================
void setup() {
    // 1. Inizializza la seriale di Debug (USB)
    Serial.begin(115200);
    Serial.println("\n[SmartVase Hub] Avvio... v1.0");

    // 2. Inizializza il gestore della configurazione NVS
    // Questo è il primo passo FONDAMENTALE.
    if (!configManager.init()) {
        Serial.println("[CRITICAL] Impossibile inizializzare NVS. Riavvio.");
        delay(2000);
        ESP.restart();
    }
    configManager.loadConfig(); // Carica o crea la configurazione di default

    // 3. Inizializza e avvia il Wi-Fi
    // Questa funzione è bloccante: o si connette, o avvia
    // l'Access Point di provisioning e attende lì.
    wifiManager.connect();
    Serial.println("[SETUP] Connessione Wi-Fi stabilita.");

    // 4. Inizializzazione altri moduli (RTC, Serial1 per Mega, etc.)
    // ... (da aggiungere)

    // 5. Creazione dei Task FreeRTOS
    // Questi task inizieranno a girare in parallelo dopo il setup.
    
    // Task per la comunicazione con il Mega (Core 1)
    xTaskCreatePinnedToCore(
        taskSerialMega,     // Funzione del task
        "TaskSerialMega",   // Nome (per debug)
        4096,               // Stack size (bytes)
        NULL,               // Parametri
        3,                  // Priorità (alta)
        NULL,               // Handle
        1);                 // Core 1 (dedicato alle periferiche)

    // Task per la comunicazione MQTT (Core 0)
    xTaskCreatePinnedToCore(
        taskMqttLink,       // Funzione del task
        "TaskMqttLink",     // Nome
        4096,               // Stack size
        NULL,               // Parametri
        2,                  // Priorità (media)
        NULL,               // Handle
        0);                 // Core 0 (dedicato alla connettività)

    // Task per la logica principale (Core 0)
    xTaskCreatePinnedToCore(
        taskMainLogic,      // Funzione del task
        "TaskMainLogic",    // Nome
        4096,               // Stack size
        NULL,               // Parametri
        1,                  // Priorità (bassa)
        NULL,               // Handle
        0);                 // Core 0

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


// =================================================================
// DEFINIZIONI DEI TASK
// =================================================================

/**
 * @brief Task #1: Gestione Seriale con Mega
 * * Si occupa di:
 * - Leggere i frame Protobuf da Serial1 (UART per il Mega)
 * - Decodificare i messaggi (Telemetry, Log, Heartbeat)
 * - Mettere i messaggi decodificati in una Coda (Queue) per il TaskMainLogic
 * - Rimanere in attesa di comandi da inviare (da un'altra Coda)
 * - Inviare comandi Protobuf al Mega
 */
void taskSerialMega(void *pvParameters) {
    Serial.println("[TaskSerialMega] Avviato.");
    // Inizializza Serial1 per il Mega
    // ...
    for(;;) {
        // Logica robusta di lettura frame (SOF, len, CRC, decode)
        // ...

        // Non bloccare mai!
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

/**
 * @brief Task #2: Gestione Connessione MQTT
 * * Si occupa di:
 * - Connettersi e rimanere connesso al broker MQTT
 * - Sottoscriversi ai topic di comando (es. .../command/config)
 * - Gestire i messaggi in ingresso e metterli in una Coda
 * - Prendere messaggi (telemetria, log) da una Coda e pubblicarli
 */
void taskMqttLink(void *pvParameters) {
    Serial.println("[TaskMqttLink] Avviato.");
    // Inizializza il client MQTT
    // ...
    for(;;) {
        // loop() del client MQTT (gestisce keepalive, etc.)
        // ...

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Task #3: Logica Principale (Orchestratore)
 * * Si occupa di:
 * - Controllare la Coda di messaggi dal Mega
 * - Tradurre i messaggi Protobuf in JSON
 * - Passare i JSON alla Coda di pubblicazione MQTT
 * - Controllare la Coda di comandi da MQTT
 * - Tradurre i comandi JSON in Protobuf
 * - Passare i Protobuf alla Coda di invio SerialMega
 * - Gestire i "deadman switch" e la logica degli Allarmi
 */
void taskMainLogic(void *pvParameters) {
    Serial.println("[TaskMainLogic] Avviato.");
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // Esegui logica ogni secondo

    for(;;) {
        // Attendi per un intervallo fisso
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        
        // Esegui logica di controllo (deadman switch, allarmi, etc.)
        // ...
    }
}
