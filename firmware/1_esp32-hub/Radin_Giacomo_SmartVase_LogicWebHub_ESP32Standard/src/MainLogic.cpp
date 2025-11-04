#include "MainLogic.h"
#include "esp_log.h"
#include <ArduinoJson.h> // Per la creazione dei JSON
#include <time.h>        // Per timestamp (se necessario)
#include <esp_wifi.h>    // Per esp_wifi_get_mac

// Tag per i log di questo modulo
static const char *TAG = "MainLogic";

// Intervallo invio telemetria MQTT (in ms) - Corrisponde alla nostra specifica
#define TELEMETRY_PUBLISH_INTERVAL_MS (60 * 1000)
// Timeout Deadman Switch per il Mega (in ms)
// Deve essere leggermente SUPERIORE al timeout definito nel Mega (HUB_DEADMAN_TIMEOUT_MS)
// per evitare falsi positivi dovuti a piccole latenze.
// Assumiamo che HUB_DEADMAN_TIMEOUT_MS nel Mega sia 120000 ms.
#define MEGA_HEARTBEAT_TIMEOUT_MS (130 * 1000) // 130 secondi (10s di margine)

// --- Implementazione Metodi Classe MainLogic ---

MainLogic::MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
                     QueueHandle_t mqttTxQueue, ConfigManager& configManager)
    : _serialRxQueue(serialRxQueue),
      _serialTxQueue(serialTxQueue),
      _mqttTxQueue(mqttTxQueue),
      _configManager(configManager),
      _telemetryTimer(NULL),
      _lastMegaHeartbeatMs(0),
      _isMegaConnected(false), // Inizia come disconnesso
      _lastBatteryVoltage(0.0f) // Inizializza
{
    // Inizializza le strutture di telemetria a zero
    memset(&_lastFastTelemetry, 0, sizeof(TelemetryFast));
    memset(&_lastDeepTelemetry, 0, sizeof(TelemetryDeep));
    // Il costruttore riceve le dipendenze (code, config)
}

void MainLogic::init() {
    ESP_LOGI(TAG, "Initializing Main Logic...");
    _lastMegaHeartbeatMs = millis(); // Inizializza il timer del deadman

    // Crea il timer FreeRTOS per l'invio periodico della telemetria
    _telemetryTimer = xTimerCreate(
        "TelemetryTimer",                 // Nome (per debug)
        pdMS_TO_TICKS(TELEMETRY_PUBLISH_INTERVAL_MS), // Periodo in tick
        pdTRUE,                           // Auto-reload (periodico)
        (void*)this,                      // ID Timer (passiamo puntatore a questa istanza)
        MainLogic::telemetryTimerCallback // Funzione callback statica
    );

    if (_telemetryTimer == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry timer!");
    } else {
        if (xTimerStart(_telemetryTimer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start telemetry timer!");
        } else {
            ESP_LOGI(TAG, "Telemetry timer started (interval: %d ms).", TELEMETRY_PUBLISH_INTERVAL_MS);
        }
    }
}

// Funzione statica entry point per il Task FreeRTOS
void MainLogic::taskEntry(void* pvParameters) {
    MainLogic* instance = static_cast<MainLogic*>(pvParameters);
    instance->init(); // Chiama l'inizializzazione specifica
    instance->taskRun(); // Entra nel loop principale del task
}

// Funzione principale del Task (loop infinito)
void MainLogic::taskRun() {
    ESP_LOGI(TAG, "MainLogic Task Started.");
    SerialMessage receivedMsg;

    while (true) {
        // 1. Aspetta (bloccante) un messaggio dalla coda seriale RX
        //    con un timeout per permettere altri controlli periodici
        if (xQueueReceive(_serialRxQueue, &receivedMsg, pdMS_TO_TICKS(1000)) == pdPASS) {
            // Messaggio ricevuto dal Mega, processalo
            processSerialMessage(receivedMsg.message);
        }

        // 2. Controlli periodici (eseguiti ogni secondo circa)
        checkMegaConnection();
        applyDefaultPlantLogic(); // Placeholder

        // Altri controlli periodici qui...
    } // Fine while(true)
}

// Gestisce un messaggio Protobuf ricevuto dal Mega
void MainLogic::processSerialMessage(const WrapperMessage& msg) {
    ESP_LOGD(TAG, "Processing message received from Mega (type: %d)", msg.which_payload);

    // Aggiorna il timestamp dell'ultimo heartbeat se è un messaggio valido
    // Indipendentemente dal tipo di messaggio, la sua ricezione conferma che il Mega è vivo.
    _lastMegaHeartbeatMs = millis();
    if (!_isMegaConnected) {
        _isMegaConnected = true;
        ESP_LOGI(TAG, "Mega connection RE-ESTABLISHED (message received).");
        // Potremmo inviare un allarme "Mega Connected" qui, se utile
    }

    switch (msg.which_payload) {
        case WrapperMessage_telemetry_fast_tag:
            ESP_LOGD(TAG, "Received Fast Telemetry: Front=%.1f", msg.payload.telemetry_fast.front_dist_cm);
            _lastFastTelemetry = msg.payload.telemetry_fast;
            break;

        case WrapperMessage_telemetry_deep_tag:
            ESP_LOGI(TAG, "Received Deep Telemetry: Uptime=%lu s, RAM=%lu B, VBat=%.2f V",
                     msg.payload.telemetry_deep.uptime_s,
                     msg.payload.telemetry_deep.free_ram_bytes,
                     msg.payload.telemetry_deep.battery_voltage); // Assumendo che battery_voltage sia nel proto v3
            _lastDeepTelemetry = msg.payload.telemetry_deep;
            _lastBatteryVoltage = msg.payload.telemetry_deep.battery_voltage; // Salva specificamente il voltaggio

            // --- Logica Allarmi basata su Deep Telemetry ---
            // Esempio: Rileva un reset da watchdog del Mega
            static uint32_t lastReportedWdtResets = 0; // Memoria statica per evitare allarmi ripetuti
            if (msg.payload.telemetry_deep.watchdog_resets > lastReportedWdtResets) {
                 publishAlarmJson("MegaWDTReset", "Mega rebooted by watchdog");
                 lastReportedWdtResets = msg.payload.telemetry_deep.watchdog_resets; // Aggiorna il contatore
            }
            // Aggiungi qui altri controlli per allarmi (es. batteria bassa del Mega se < soglia)
            // if (_lastBatteryVoltage < BATTERY_LOW_THRESHOLD) { ... }
            break;

        case WrapperMessage_log_tag:
            // Log ricevuti dal Mega vengono inoltrati a MQTT
            publishLogJson(msg.payload.log);
            break;

        case WrapperMessage_heartbeat_tag:
            ESP_LOGD(TAG, "Received Heartbeat from Mega. Uptime=%lu", msg.payload.heartbeat.uptime_s);
            // L'heartbeat aggiorna solo _lastMegaHeartbeatMs (fatto all'inizio della funzione)
            break;

        case WrapperMessage_command_response_tag:
            ESP_LOGI(TAG, "Received Command Response from Mega: ID=%lu, Status=%d, Detail=%s",
                     msg.payload.command_response.cmd_id, msg.payload.command_response.status, msg.payload.command_response.detail);
            // TODO: In futuro, potremmo usare il cmd_id per correlare questa risposta
            //       a un comando inviato (magari originato da MQTT) e notificare il successo/fallimento.
            break;

        default:
            ESP_LOGW(TAG, "Received unknown message type from Mega: %d", msg.which_payload);
            break;
    }
}

// Callback statico per il timer della telemetria
void MainLogic::telemetryTimerCallback(TimerHandle_t xTimer) {
    MainLogic* instance = static_cast<MainLogic*>(pvTimerGetTimerID(xTimer));
    if (instance) {
        ESP_LOGI(TAG, "Telemetry timer triggered. Publishing data...");
        // Recupera gli ultimi dati salvati dalle variabili membro
        TelemetryFast lastFastData = instance->_lastFastTelemetry;
        TelemetryDeep lastDeepData = instance->_lastDeepTelemetry;
        instance->publishTelemetryJson(lastFastData, lastDeepData);
    }
}

// Controlla il deadman switch del Mega
void MainLogic::checkMegaConnection() {
    if (_isMegaConnected) {
        // Se è passato troppo tempo dall'ultimo messaggio/heartbeat
        if (millis() - _lastMegaHeartbeatMs > MEGA_HEARTBEAT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Mega connection LOST (timeout)! Last message %lu ms ago.", millis() - _lastMegaHeartbeatMs);
            _isMegaConnected = false; // Aggiorna lo stato
            publishAlarmJson("MegaDisconnected", "Timeout receiving messages"); // Invia allarme
            // TODO: Aggiungere logica di "safe mode" se necessario (es. fermare task MQTT?)
        }
    }
    // Nessun else: se _isMegaConnected è false, attendiamo passivamente il prossimo messaggio
    // per considerarlo di nuovo connesso (logica in processSerialMessage).
}

// Converte la telemetria Protobuf in JSON e la invia alla coda MQTT
void MainLogic::publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td) {
    ESP_LOGD(TAG, "Preparing Telemetry JSON...");

    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;

    // Campi allineati con SmartVase_data_structure.md
    doc["timestamp_utc"] = millis(); // Usiamo millis() come placeholder, ideale sarebbe un RTC
    doc["uptime_s"] = td.uptime_s;
    if (!isnan(_lastBatteryVoltage)) {
        doc["battery_voltage"] = round(_lastBatteryVoltage * 100) / 100.0;
    }
    if (!isnan(tf.water_level_cm)) {
        doc["water_level_cm"] = round(tf.water_level_cm * 10) / 10.0;
    }
    // 'soil_moisture' non è presente nei dati Protobuf ricevuti, andrà aggiunto quando disponibile
    if (!isnan(td.temperature_c)) {
        doc["temperature_c"] = round(td.temperature_c * 10) / 10.0;
    }
    if (!isnan(td.humidity_percent)) {
        doc["humidity_percent"] = round(td.humidity_percent * 10) / 10.0;
    }
    if (tf.lux >= 0) {
        doc["lux"] = tf.lux;
    }
    doc["device_status"] = _isMegaConnected ? "NOMINAL" : "DEGRADED_NO_MEGA";

    // Campi aggiuntivi di diagnostica (non nella specifica ma utili)
    JsonObject diagnostics = doc.createNestedObject("diagnostics");
    diagnostics["mega_connected"] = _isMegaConnected;
    diagnostics["mega_ram_bytes"] = td.free_ram_bytes;
    diagnostics["mega_wdt_resets"] = td.watchdog_resets;
    diagnostics["hub_free_heap_bytes"] = ESP.getFreeHeap();
    diagnostics["movement_state"] = tf.movement_state;


    MqttMessage mqttMsg;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mqttMsg.topic, sizeof(mqttMsg.topic), "smartvase/HUB_%02X%02X%02X/telemetry", mac[3], mac[4], mac[5]);

    size_t len = serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
    if (len >= sizeof(mqttMsg.payload)) {
        ESP_LOGE(TAG, "Telemetry JSON buffer too small! Truncated. Needed: %d, Available: %d", serializeJson(doc, NULL, 0), sizeof(mqttMsg.payload));
        return;
    } else if (len == 0) {
         ESP_LOGE(TAG, "Telemetry JSON serialization failed!");
         return;
    }
    mqttMsg.payload[len] = '0';

    ESP_LOGD(TAG, "Sending Telemetry JSON to MQTT Queue: Topic='%s', Payload_len=%d", mqttMsg.topic, len);

    if (xQueueSend(_mqttTxQueue, &mqttMsg, (TickType_t)0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX Queue is full! Discarding telemetry message.");
    }
}

// Converte un log Protobuf in JSON e lo invia alla coda MQTT
void MainLogic::publishLogJson(const Log& log) {
     ESP_LOGD(TAG, "Preparing Log JSON...");

    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;

    doc["timestamp_utc"] = log.timestamp_ms ? log.timestamp_ms : millis();
    switch(log.level) {
        case Log_LogLevel_INFO: doc["level"] = "INFO"; break;
        case Log_LogLevel_WARN: doc["level"] = "WARN"; break;
        case Log_LogLevel_ERROR: doc["level"] = "ERROR"; break;
        case Log_LogLevel_CRITICAL: doc["level"] = "CRITICAL"; break;
        default: doc["level"] = "UNKNOWN"; break;
    }
    doc["source_device"] = log.source_device;
    doc["event"] = log.event;
    if (strlen(log.detail) > 0) {
        doc["detail"] = log.detail;
    }

    MqttMessage mqttMsg;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mqttMsg.topic, sizeof(mqttMsg.topic), "smartvase/HUB_%02X%02X%02X/logs", mac[3], mac[4], mac[5]);

    size_t len = serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
    if (len >= sizeof(mqttMsg.payload)) {
        ESP_LOGE(TAG, "Log JSON buffer too small! Truncated.");
        return;
    } else if (len == 0) {
         ESP_LOGE(TAG, "Log JSON serialization failed!");
         return;
    }
    mqttMsg.payload[len] = '0';

    ESP_LOGD(TAG, "Sending Log JSON to MQTT Queue: Topic='%s', Payload_len=%d", mqttMsg.topic, len);

    if (xQueueSend(_mqttTxQueue, &mqttMsg, (TickType_t)0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX Queue is full! Discarding log message.");
    }
}

// Invia un allarme JSON alla coda MQTT
void MainLogic::publishAlarmJson(const char* alarmType, const char* detail) {
    ESP_LOGW(TAG, "ALARM PUBLISH: Type='%s', Detail='%s'", alarmType, detail);

    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;

    doc["timestamp_utc"] = millis();
    doc["type"] = alarmType;
    if (detail) {
        doc["detail"] = detail;
    }
    
    // Aggiungi dati di contesto utili
    JsonObject context = doc.createNestedObject("context");
    context["mega_connected"] = _isMegaConnected;
    if (!isnan(_lastBatteryVoltage)) {
        context["battery_voltage"] = round(_lastBatteryVoltage * 100) / 100.0;
    }

    MqttMessage mqttMsg;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mqttMsg.topic, sizeof(mqttMsg.topic), "smartvase/HUB_%02X%02X%02X/alarms", mac[3], mac[4], mac[5]);

    size_t len = serializeJson(doc, mqttMsg.payload, sizeof(mqttMsg.payload));
    if (len >= sizeof(mqttMsg.payload)) {
        ESP_LOGE(TAG, "Alarm JSON buffer too small! Truncated.");
        return;
    } else if (len == 0) {
         ESP_LOGE(TAG, "Alarm JSON serialization failed!");
         return;
    }
    mqttMsg.payload[len] = '0';

    ESP_LOGD(TAG, "Sending Alarm JSON to MQTT Queue: Topic='%s', Payload_len=%d", mqttMsg.topic, len);

    // Usa un timeout piccolo per dare priorità agli allarmi
    if (xQueueSend(_mqttTxQueue, &mqttMsg, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGE(TAG, "MQTT TX Queue full! DISCARDING ALARM: %s", alarmType);
    }
}

// Invia un comando Protobuf al Mega (lo mette nella coda serialTxQueue)
void MainLogic::sendCommandToMega(const Command& cmd) {
    // Genera un ID comando univoco (basato su millis() è semplice, non garantito univoco al 100% ma sufficiente per ora)
    uint32_t cmdId = millis();

    // Crea il messaggio wrapper
    SerialMessage msgToSend;
    memset(&msgToSend.message, 0, sizeof(WrapperMessage)); // Pulisce
    msgToSend.message.which_payload = WrapperMessage_command_tag;
    // Copia il comando specifico nel payload
    memcpy(&msgToSend.message.payload.command, &cmd, sizeof(Command));
    // Assegna l'ID comando
    msgToSend.message.payload.command.cmd_id = cmdId; // Assumendo che cmd_id sia nel Command

    ESP_LOGI(TAG, "Queueing command for Mega (ID: %lu, Type: %d)...", cmdId, cmd.which_command_type);

    // Invia alla coda _serialTxQueue, con timeout
    if (xQueueSend(_serialTxQueue, &msgToSend, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Serial TX Queue is full! Failed to send command ID %lu to Mega.", cmdId);
        // Potremmo inviare un allarme qui? O ritentare?
    }
}


// Implementa la logica di default per la pianta (placeholder)
void MainLogic::applyDefaultPlantLogic() {
    static uint32_t lastWaterCheck = 0;
    const uint32_t waterCheckInterval = 12 * 60 * 60 * 1000; // Ogni 12 ore

    // Esegui solo se il Mega è connesso
    if (!_isMegaConnected) return;

    if (millis() - lastWaterCheck > waterCheckInterval) {
        ESP_LOGI(TAG, "Performing default plant logic check...");
        lastWaterCheck = millis();

        // 1. Richiedi lettura umidità terreno al Mega
        Command cmdSoil = Command_init_zero;
        cmdSoil.which_command_type = Command_read_soil_tag;
        sendCommandToMega(cmdSoil);
        ESP_LOGI(TAG, "Requested soil reading from Mega.");

        // TODO: La logica di irrigazione basata sulla risposta è più complessa.
        // Richiede di salvare lo stato "attesa lettura suolo" e processare
        // la risposta (Log con event="soil_reading") in processSerialMessage.
        // Per ora, ci limitiamo a richiedere la lettura.

        // TODO: Logica Luce (placeholder)
        // Recupera l'ultimo valore lux (dovrebbe essere in una variabile membro)
        // float lastLux = this->_lastFastTelemetry.lux;
        // if (lastLux < 400 && lastLux >= 0) { ... send UVA ON command ... }
        // else if (lastLux > 600) { ... send UVA OFF command ... }
    }
}

