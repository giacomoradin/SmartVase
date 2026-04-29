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
                     QueueHandle_t mqttTxQueue, QueueHandle_t mqttRxQueue, ConfigManager& configManager)
    : _serialRxQueue(serialRxQueue),
      _serialTxQueue(serialTxQueue),
      _mqttTxQueue(mqttTxQueue),
      _mqttRxQueue(mqttRxQueue),
      _configManager(configManager),
      _telemetryTimer(NULL),
      _lastMegaHeartbeatMs(0),
      _isMegaConnected(false),
      _lastBatteryVoltage(0.0f)
{
    memset(&_lastFastTelemetry, 0, sizeof(TelemetryFast));
    memset(&_lastDeepTelemetry, 0, sizeof(TelemetryDeep));
}

void MainLogic::init() {
    ESP_LOGI(TAG, "Initializing Main Logic...");
    _lastMegaHeartbeatMs = millis();

    _telemetryTimer = xTimerCreate(
        "TelemetryTimer",
        pdMS_TO_TICKS(TELEMETRY_PUBLISH_INTERVAL_MS),
        pdTRUE,
        (void*)this,
        MainLogic::telemetryTimerCallback
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
    instance->init();
    instance->taskRun();
}

// Funzione principale del Task (loop infinito)
void MainLogic::taskRun() {
    ESP_LOGI(TAG, "MainLogic Task Started.");
    SerialMessage receivedSerialMsg;
    MqttCommand receivedMqttCmd;

    while (true) {
        // 1. Processa messaggi dalla coda seriale RX (dal Mega)
        if (xQueueReceive(_serialRxQueue, &receivedSerialMsg, pdMS_TO_TICKS(50)) == pdPASS) {
            processSerialMessage(receivedSerialMsg.message);
        }

        // 2. Processa comandi dalla coda MQTT RX (dal MqttManager)
        if (xQueueReceive(_mqttRxQueue, &receivedMqttCmd, pdMS_TO_TICKS(50)) == pdPASS) {
            processMqttCommand(receivedMqttCmd);
        }

        // 3. Controlli periodici
        checkMegaConnection();
        applyDefaultPlantLogic();

        // Cede la CPU per un breve periodo
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Gestisce un comando JSON ricevuto via MQTT
void MainLogic::processMqttCommand(const MqttCommand& cmd) {
    ESP_LOGI(TAG, "Processing MQTT Command from topic: %s", cmd.topic);
    ESP_LOGD(TAG, "Payload: %s", cmd.payload);

    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;
    DeserializationError error = deserializeJson(doc, cmd.payload);

    if (error) {
        ESP_LOGE(TAG, "deserializeJson() failed for MQTT command: %s", error.c_str());
        // TODO: Inviare una risposta di errore via MQTT?
        return;
    }

    const char* commandType = doc["type"];
    if (!commandType) {
        ESP_LOGW(TAG, "MQTT command without 'type' field.");
        // TODO: Inviare una risposta di errore via MQTT?
        return;
    }

    // Inizializza un comando Protobuf da inviare al Mega
    Command megaCmd = Command_init_zero;

    if (strcmp(commandType, "setMode") == 0) {
        const char* modeStr = doc["mode"];
        if (modeStr) {
            megaCmd.which_command_type = Command_set_mode_tag;
            if (strcmp(modeStr, "IDLE") == 0) {
                megaCmd.command_type.set_mode.mode = smartvase_SetModeCommand_Mode_IDLE;
            } else if (strcmp(modeStr, "LIGHT") == 0) {
                megaCmd.command_type.set_mode.mode = smartvase_SetModeCommand_Mode_LIGHT;
            } else if (strcmp(modeStr, "SHADOW") == 0) {
                megaCmd.command_type.set_mode.mode = smartvase_SetModeCommand_Mode_SHADOW;
            } else {
                ESP_LOGW(TAG, "Unknown mode for setMode command: %s", modeStr);
                return;
            }
            sendCommandToMega(megaCmd);
            ESP_LOGI(TAG, "Sent setMode command to Mega: %s", modeStr);
        }
    } else if (strcmp(commandType, "water") == 0) {
        if (doc.containsKey("duration_ms")) {
            megaCmd.which_command_type = Command_water_tag;
            megaCmd.command_type.water.duration_ms = doc["duration_ms"];
            sendCommandToMega(megaCmd);
            ESP_LOGI(TAG, "Sent water command to Mega for %lu ms", megaCmd.command_type.water.duration_ms);
        }
    } else if (strcmp(commandType, "stop") == 0) {
        megaCmd.which_command_type = Command_stop_tag;
        sendCommandToMega(megaCmd);
        ESP_LOGI(TAG, "Sent stop command to Mega.");
    } else if (strcmp(commandType, "requestDiagnostics") == 0) {
        megaCmd.which_command_type = Command_request_diagnostics_tag;
        sendCommandToMega(megaCmd);
        ESP_LOGI(TAG, "Sent requestDiagnostics command to Mega.");
    } else if (strcmp(commandType, "setMotionParams") == 0) {
        if (doc.containsKey("reverse_ms") && doc.containsKey("turn_ms")) {
            megaCmd.which_command_type = Command_set_motion_params_tag;
            megaCmd.command_type.set_motion_params.reverse_ms = doc["reverse_ms"];
            megaCmd.command_type.set_motion_params.turn_ms = doc["turn_ms"];
            sendCommandToMega(megaCmd);
            ESP_LOGI(TAG, "Sent setMotionParams command to Mega: reverse=%lu, turn=%lu", megaCmd.command_type.set_motion_params.reverse_ms, megaCmd.command_type.set_motion_params.turn_ms);
        }
    } else if (strcmp(commandType, "readSoil") == 0) {
        megaCmd.which_command_type = Command_read_soil_tag;
        sendCommandToMega(megaCmd);
        ESP_LOGI(TAG, "Sent readSoil command to Mega.");
    } else if (strcmp(commandType, "softReset") == 0) {
        megaCmd.which_command_type = Command_soft_reset_tag;
        sendCommandToMega(megaCmd);
        ESP_LOGI(TAG, "Sent softReset command to Mega.");
    } else {
        ESP_LOGW(TAG, "Unknown MQTT command type: %s", commandType);
        // TODO: Inviare una risposta di errore via MQTT?
    }
}

// --- Implementazioni Funzioni Mancanti ---

void MainLogic::processSerialMessage(const WrapperMessage& msg) {
    ESP_LOGI(TAG, "Processing Serial Message");
}

void MainLogic::publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td) {
    ESP_LOGI(TAG, "Publishing Telemetry");
}

void MainLogic::publishLogJson(const Log& log) {
    ESP_LOGI(TAG, "Publishing Log");
}

void MainLogic::publishAlarmJson(const char* alarmType, const char* detail) {
    ESP_LOGI(TAG, "Publishing Alarm");
}

void MainLogic::sendCommandToMega(const Command& cmd) {
    ESP_LOGI(TAG, "Sending Command to Mega");
    SerialMessage msg;
    msg.message.which_payload = WrapperMessage_command_tag;
    msg.message.payload.command = cmd;
    if (xQueueSend(_serialTxQueue, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to queue command for Mega. TX Queue may be full.");
    }
}

void MainLogic::telemetryTimerCallback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Telemetry Timer Callback");
    MainLogic* instance = static_cast<MainLogic*>(pvTimerGetTimerID(xTimer));
    instance->publishTelemetryJson(instance->_lastFastTelemetry, instance->_lastDeepTelemetry);
}

void MainLogic::checkMegaConnection() {
    //ESP_LOGI(TAG, "Checking Mega Connection");
}

void MainLogic::applyDefaultPlantLogic() {
    //ESP_LOGI(TAG, "Applying Default Plant Logic");
}
