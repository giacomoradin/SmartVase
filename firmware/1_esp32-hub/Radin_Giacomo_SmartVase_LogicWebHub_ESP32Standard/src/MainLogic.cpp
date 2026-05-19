#include "MainLogic.h"
#include "esp_log.h"
#include <ArduinoJson.h>
#include <time.h>
#include <esp_wifi.h>

static const char *TAG = "MainLogic";

// Intervalli scheduling (ms)
#define TELEMETRY_PUBLISH_INTERVAL_MS  (60 * 1000)
// Deadman switch: Mega muto per > MEGA_HEARTBEAT_TIMEOUT_MS ⇒ alarm.
// Lasciato leggermente superiore al deadman del Mega (120s) per i ritardi.
#define MEGA_HEARTBEAT_TIMEOUT_MS      (130 * 1000)

MainLogic::MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
                     QueueHandle_t mqttTxQueue, QueueHandle_t mqttRxQueue,
                     ConfigManager& configManager)
    : _serialRxQueue(serialRxQueue),
      _serialTxQueue(serialTxQueue),
      _mqttTxQueue(mqttTxQueue),
      _mqttRxQueue(mqttRxQueue),
      _configManager(configManager),
      _telemetryTimer(NULL),
      _lastMegaHeartbeatMs(0),
      _isMegaConnected(false)
{
    memset(&_lastFastTelemetry, 0, sizeof(TelemetryFast));
    memset(&_lastDeepTelemetry, 0, sizeof(TelemetryDeep));
    // device_id derivato dal MAC al primo uso
    _deviceId[0] = '\0';
}

void MainLogic::init() {
    ESP_LOGI(TAG, "Initializing Main Logic...");
    _lastMegaHeartbeatMs = millis();

    // Genera device_id "HUB_XXXXXX" dagli ultimi 3 byte del MAC.
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(_deviceId, sizeof(_deviceId), "HUB_%02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", _deviceId);

    _telemetryTimer = xTimerCreate(
        "TelemetryTimer",
        pdMS_TO_TICKS(TELEMETRY_PUBLISH_INTERVAL_MS),
        pdTRUE,
        (void*)this,
        MainLogic::telemetryTimerCallback);

    if (_telemetryTimer == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry timer!");
    } else if (xTimerStart(_telemetryTimer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start telemetry timer!");
    } else {
        ESP_LOGI(TAG, "Telemetry timer started (interval: %d ms).", TELEMETRY_PUBLISH_INTERVAL_MS);
    }
}

void MainLogic::taskEntry(void* pvParameters) {
    // init() e' chiamato dal setup() prima della creazione del task,
    // quindi qui non lo richiamiamo per evitare doppia inizializzazione del timer.
    MainLogic* instance = static_cast<MainLogic*>(pvParameters);
    instance->taskRun();
}

void MainLogic::taskRun() {
    ESP_LOGI(TAG, "MainLogic Task Started.");
    SerialMessage receivedSerialMsg;
    MqttCommand   receivedMqttCmd;

    while (true) {
        if (xQueueReceive(_serialRxQueue, &receivedSerialMsg, pdMS_TO_TICKS(50)) == pdPASS) {
            processSerialMessage(receivedSerialMsg.message);
        }
        if (xQueueReceive(_mqttRxQueue, &receivedMqttCmd, pdMS_TO_TICKS(50)) == pdPASS) {
            processMqttCommand(receivedMqttCmd);
        }

        checkMegaConnection();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =================================================================
// MQTT command → Mega Protobuf
// =================================================================
void MainLogic::processMqttCommand(const MqttCommand& cmd) {
    ESP_LOGI(TAG, "MQTT command from %s", cmd.topic);
    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;
    DeserializationError err = deserializeJson(doc, cmd.payload);
    if (err) {
        ESP_LOGE(TAG, "deserializeJson failed: %s", err.c_str());
        publishAlarmJson("bad_command_json", err.c_str());
        return;
    }

    const char* type = doc["type"];
    if (!type) {
        ESP_LOGW(TAG, "MQTT command without 'type'");
        return;
    }

    Command megaCmd = Command_init_zero;
    megaCmd.cmd_id  = doc["cmd_id"] | 0;

    if (strcmp(type, "setMode") == 0) {
        const char* modeStr = doc["mode"] | "";
        megaCmd.which_command_type = Command_set_mode_tag;
        if      (strcmp(modeStr, "LIGHT")  == 0) megaCmd.command_type.set_mode.mode = SetModeCommand_Mode_LIGHT;
        else if (strcmp(modeStr, "SHADOW") == 0) megaCmd.command_type.set_mode.mode = SetModeCommand_Mode_SHADOW;
        else                                     megaCmd.command_type.set_mode.mode = SetModeCommand_Mode_IDLE;
    } else if (strcmp(type, "water") == 0) {
        megaCmd.which_command_type = Command_water_tag;
        megaCmd.command_type.water.duration_ms = doc["duration_ms"] | 0;
    } else if (strcmp(type, "stop") == 0) {
        megaCmd.which_command_type = Command_stop_tag;
    } else if (strcmp(type, "requestDiagnostics") == 0) {
        megaCmd.which_command_type = Command_request_diagnostics_tag;
    } else if (strcmp(type, "setMotionParams") == 0) {
        megaCmd.which_command_type = Command_set_motion_params_tag;
        megaCmd.command_type.set_motion_params.reverse_ms = doc["reverse_ms"] | 1000;
        megaCmd.command_type.set_motion_params.turn_ms    = doc["turn_ms"]    | 1200;
    } else if (strcmp(type, "readSoil") == 0) {
        megaCmd.which_command_type = Command_read_soil_tag;
    } else if (strcmp(type, "softReset") == 0) {
        megaCmd.which_command_type = Command_soft_reset_tag;
    } else {
        ESP_LOGW(TAG, "Unknown command type: %s", type);
        publishAlarmJson("unknown_command_type", type);
        return;
    }

    sendCommandToMega(megaCmd);
}

void MainLogic::sendCommandToMega(const Command& cmd) {
    SerialMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.message.which_payload   = WrapperMessage_command_tag;
    msg.message.payload.command = cmd;
    if (xQueueSend(_serialTxQueue, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "TX queue full, command dropped");
        publishAlarmJson("tx_queue_full", "command_dropped");
    }
}

// =================================================================
// Mega Protobuf → JSON MQTT
// =================================================================
void MainLogic::processSerialMessage(const WrapperMessage& msg) {
    _lastMegaHeartbeatMs = millis();
    if (!_isMegaConnected) {
        _isMegaConnected = true;
        publishAlarmJson("mega_online", "heartbeat_resumed");
    }

    switch (msg.which_payload) {
        case WrapperMessage_telemetry_fast_tag:
            _lastFastTelemetry = msg.payload.telemetry_fast;
            // Pubblicazione opportunistica: la telemetria periodica e' gestita dal timer
            // ma il primo aggiornamento dopo un reset viene pubblicato subito.
            break;
        case WrapperMessage_telemetry_deep_tag:
            _lastDeepTelemetry = msg.payload.telemetry_deep;
            publishTelemetryJson(_lastFastTelemetry, _lastDeepTelemetry);
            break;
        case WrapperMessage_log_tag:
            publishLogJson(msg.payload.log);
            break;
        case WrapperMessage_heartbeat_tag:
            // Solo aggiornamento timestamp (gia' fatto sopra). Non si pubblica.
            break;
        case WrapperMessage_command_response_tag:
            publishCommandAckJson(msg.payload.command_response);
            break;
        default:
            ESP_LOGW(TAG, "Unknown serial payload tag: %d", msg.which_payload);
            break;
    }
}

static const char* movementStateToStr(MovementState s) {
    switch (s) {
        case MS_MOVING:          return "MOVING";
        case MS_AVOID_START:     return "AVOID_START";
        case MS_AVOID_REVERSING: return "AVOID_REVERSING";
        case MS_AVOID_TURNING:   return "AVOID_TURNING";
        case MS_STUCK:           return "STUCK";
        case MS_IDLE:
        default:                 return "IDLE";
    }
}

static const char* logLevelToStr(Log_LogLevel l) {
    switch (l) {
        case Log_LogLevel_WARN:     return "WARN";
        case Log_LogLevel_ERROR:    return "ERROR";
        case Log_LogLevel_CRITICAL: return "CRITICAL";
        case Log_LogLevel_INFO:
        default:                    return "INFO";
    }
}

void MainLogic::publishTelemetryJson(const TelemetryFast& tf, const TelemetryDeep& td) {
    MqttMessage out;
    snprintf(out.topic, sizeof(out.topic), "smartvase/%s/telemetry", _deviceId);

    StaticJsonDocument<MAX_JSON_MESSAGE_LENGTH> doc;
    doc["timestamp_utc"]    = tf.epoch_s != 0 ? tf.epoch_s : td.epoch_s;
    doc["uptime_s"]         = td.uptime_s;
    doc["device_id"]        = _deviceId;
    doc["movement_state"]   = movementStateToStr(tf.movement_state);
    doc["lux"]              = tf.lux;
    doc["soil_moisture"]    = tf.soil_moisture;
    doc["water_level_cm"]   = tf.water_level_cm;
    JsonObject d = doc.createNestedObject("distances_cm");
    d["top"]         = tf.top_dist_cm;
    d["front_right"] = tf.front_right_dist_cm;
    d["front_left"]  = tf.front_left_dist_cm;
    d["left"]        = tf.left_dist_cm;
    d["right"]       = tf.right_dist_cm;

    doc["temperature_c"]      = td.temperature_c;
    doc["humidity_percent"]   = td.humidity_percent;
    doc["pressure_hpa"]       = td.pressure_hpa;
    doc["gas_resistance_ohms"]= td.gas_resistance_ohms;
    doc["battery_voltage"]    = td.battery_voltage;
    doc["free_ram_bytes"]     = td.free_ram_bytes;

    JsonObject c = doc.createNestedObject("counters");
    c["watchdog_resets"]              = td.watchdog_resets;
    c["total_irrigations"]            = td.total_irrigations;
    c["total_irrigation_duration_s"]  = td.total_irrigation_duration_s;
    c["total_motor_active_time_s"]    = td.total_motor_active_time_s;
    c["obstacles_avoided"]            = td.obstacles_avoided;
    c["stuck_events"]                 = td.stuck_events;
    c["bme_read_errors"]              = td.bme_read_errors;
    c["log_overflows"]                = td.log_overflows;
    c["pb_decode_failures"]           = td.pb_decode_failures;

    size_t n = serializeJson(doc, out.payload, sizeof(out.payload));
    if (n == 0 || n >= sizeof(out.payload)) {
        ESP_LOGE(TAG, "Telemetry JSON serialization failed/overflow");
        return;
    }

    if (xQueueSend(_mqttTxQueue, &out, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX queue full, telemetry dropped");
    }
}

void MainLogic::publishLogJson(const Log& log) {
    MqttMessage out;
    snprintf(out.topic, sizeof(out.topic), "smartvase/%s/logs", _deviceId);

    StaticJsonDocument<256> doc;
    doc["timestamp_ms"]  = log.timestamp_ms;
    doc["level"]         = logLevelToStr(log.level);
    doc["event"]         = log.event;
    doc["detail"]        = log.detail;
    doc["source_device"] = log.source_device[0] ? log.source_device : _deviceId;

    size_t n = serializeJson(doc, out.payload, sizeof(out.payload));
    if (n == 0 || n >= sizeof(out.payload)) return;

    if (xQueueSend(_mqttTxQueue, &out, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX queue full, log dropped");
    }
}

void MainLogic::publishCommandAckJson(const CommandResponse& r) {
    MqttMessage out;
    snprintf(out.topic, sizeof(out.topic), "smartvase/%s/command/ack", _deviceId);

    StaticJsonDocument<256> doc;
    doc["timestamp_ms"]   = (uint32_t)millis();
    doc["cmd_id"]         = r.cmd_id;
    doc["status"]         = (r.status == CommandResponse_Status_OK) ? "OK" : "ERROR";
    doc["detail"]         = r.detail;
    doc["value"]          = r.value;
    doc["exec_time_ms"]   = r.exec_time_ms;
    doc["source_device"]  = _deviceId;

    size_t n = serializeJson(doc, out.payload, sizeof(out.payload));
    if (n == 0 || n >= sizeof(out.payload)) return;

    if (xQueueSend(_mqttTxQueue, &out, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX queue full, ack dropped");
    }
}

void MainLogic::publishAlarmJson(const char* alarmType, const char* detail) {
    MqttMessage out;
    snprintf(out.topic, sizeof(out.topic), "smartvase/%s/alarm", _deviceId);

    StaticJsonDocument<256> doc;
    doc["timestamp_ms"] = (uint32_t)millis();
    doc["type"]         = alarmType ? alarmType : "unknown";
    if (detail) doc["detail"] = detail;
    doc["source_device"] = _deviceId;

    size_t n = serializeJson(doc, out.payload, sizeof(out.payload));
    if (n == 0 || n >= sizeof(out.payload)) return;

    if (xQueueSend(_mqttTxQueue, &out, pdMS_TO_TICKS(50)) != pdPASS) {
        ESP_LOGW(TAG, "MQTT TX queue full, alarm dropped");
    }
}

void MainLogic::telemetryTimerCallback(TimerHandle_t xTimer) {
    MainLogic* instance = static_cast<MainLogic*>(pvTimerGetTimerID(xTimer));
    instance->publishTelemetryJson(instance->_lastFastTelemetry, instance->_lastDeepTelemetry);
}

void MainLogic::checkMegaConnection() {
    if (millis() - _lastMegaHeartbeatMs > MEGA_HEARTBEAT_TIMEOUT_MS) {
        if (_isMegaConnected) {
            _isMegaConnected = false;
            ESP_LOGW(TAG, "Mega deadman switch triggered");
            publishAlarmJson("mega_offline", "deadman_switch_expired");
        }
    }
}
