/*! @file MainLogic.cpp
 *  @ingroup HubCore
 *  @brief Implementation of MainLogic: task loop, JSON<->Protobuf bridge,
 *  telemetry/heartbeat scheduling, deadman switch towards the Mega.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include "MainLogic.h"
#include "esp_log.h"
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <esp_wifi.h>

static const char *TAG = "MainLogic";

/*! @brief Converts a Protobuf `MovementState` into the string published in
 *  the `movement_state` field of the telemetry JSON.
 *  @param[in] s Movement state reported by the Mega.
 *  @return Constant string (e.g. "MOVING", "STUCK"); "IDLE" for any
 *  unrecognized value, including MS_IDLE itself. */
static const char* movementStateToStr(MovementState s);

/*! @brief Converts a Protobuf `Log_LogLevel` into the string published in
 *  the `level` field of the JSON logs.
 *  @param[in] l Log level reported by the Mega.
 *  @return Constant string ("WARN", "ERROR", "CRITICAL"); "INFO" for any
 *  unrecognized value, including Log_LogLevel_INFO itself. */
static const char* logLevelToStr(Log_LogLevel l);

/*! @brief Converts the Mega care-layer state code (proto v4.1
 *  `TelemetryDeep.care_state`) into the string published in the `care.state`
 *  field of the JSON telemetry.
 *  @param[in] s CareState code (0=NIGHT .. 5=TOP_UP, see the Mega's CarePolicy.h).
 *  @return Constant string; "NIGHT" for any unrecognized value. */
static const char* careStateToStr(uint32_t s);

/*! @brief Telemetry publish period over MQTT (ms). */
#define TELEMETRY_PUBLISH_INTERVAL_MS  (60 * 1000)
/*! @brief Deadman switch: Mega silent beyond this time => alarm. Kept slightly
 *         above the Mega's own deadman (120 s) to absorb delays (ms). */
#define MEGA_HEARTBEAT_TIMEOUT_MS      (130 * 1000)
/*! @brief Hub->Mega heartbeat period (ms): the Mega's deadman (120 s) trips
 *         if it receives NOTHING from the Hub, and sporadic commands alone
 *         are not enough to keep it alive. */
#define HUB_HEARTBEAT_INTERVAL_MS      (30 * 1000)
/*! @brief Hub firmware version published in telemetry (aligned with HubCli.cpp). */
#define HUB_FW_VERSION                 "1.4.0"

MainLogic::MainLogic(QueueHandle_t serialRxQueue, QueueHandle_t serialTxQueue,
                     QueueHandle_t mqttTxQueue, QueueHandle_t mqttRxQueue,
                     ConfigManager& configManager)
    : _serialRxQueue(serialRxQueue),
      _serialTxQueue(serialTxQueue),
      _mqttTxQueue(mqttTxQueue),
      _mqttRxQueue(mqttRxQueue),
      _configManager(configManager),
      _lastMegaHeartbeatMs(0),
      _isMegaConnected(false),
      _lastTelemetryPublishMs(0)
{
    memset(&_lastFastTelemetry, 0, sizeof(TelemetryFast));
    memset(&_lastDeepTelemetry, 0, sizeof(TelemetryDeep));
    // Ambient fields default to NaN until the first TelemetryDeep arrives: this
    // prevents the fallback publish from emitting 0 as if it were a real
    // reading (consistent with the omission logic in publishTelemetryJson).
    _lastDeepTelemetry.temperature_c    = NAN;
    _lastDeepTelemetry.humidity_percent = NAN;
    _lastDeepTelemetry.pressure_hpa     = NAN;
    _lastDeepTelemetry.battery_voltage  = NAN;
    _deviceId[0] = '\0';
}

void MainLogic::init() {
    ESP_LOGI(TAG, "Initializing Main Logic...");
    _lastMegaHeartbeatMs = millis();

    // STATIC device ID from the single source HUB_DEVICE_ID (ConfigManager.h),
    // consistent with MqttManager::init().
    snprintf(_deviceId, sizeof(_deviceId), "%s", HUB_DEVICE_ID);

    ESP_LOGI(TAG, "Device ID: %s", _deviceId);
}

void MainLogic::taskEntry(void* pvParameters) {
    // init() is called by setup() before the task is created, so we don't
    // call it again here, to avoid double-initializing the timer.
    MainLogic* instance = static_cast<MainLogic*>(pvParameters);
    instance->taskRun();
}

void MainLogic::taskRun() {
    ESP_LOGI(TAG, "MainLogic Task Started.");
    SerialMessage receivedSerialMsg;
    MqttCommand   receivedMqttCmd;
    uint32_t      lastHubHeartbeatMs = 0;

    while (true) {
        // Fully drain the queues (non-blocking reads): keeps latency low for
        // MQTT commands arriving in rapid succession. Pacing comes from the
        // vTaskDelay at the end of the loop, not from the receive timeouts.
        while (xQueueReceive(_serialRxQueue, &receivedSerialMsg, 0) == pdPASS) {
            processSerialMessage(receivedSerialMsg.message);
        }
        while (xQueueReceive(_mqttRxQueue, &receivedMqttCmd, 0) == pdPASS) {
            processMqttCommand(receivedMqttCmd);
        }

        // Keepalive towards the Mega: any valid frame resets its deadman,
        // so a periodic Heartbeat is enough. Since proto v4.2 the heartbeat
        // doubles as the Mega's time source: the Hub attaches its NTP epoch
        // (epoch_s) so the Mega can align its software clock (its hardware
        // RTC turned out to be faulty). 0 = "no NTP time yet" and the Mega
        // ignores the field, so an offline Hub degrades gracefully.
        if (millis() - lastHubHeartbeatMs >= HUB_HEARTBEAT_INTERVAL_MS) {
            lastHubHeartbeatMs = millis();
            SerialMessage hb;
            memset(&hb, 0, sizeof(hb));
            hb.message.which_payload = WrapperMessage_heartbeat_tag;
            hb.message.payload.heartbeat.uptime_s = millis() / 1000UL;
            hb.message.payload.heartbeat.is_degraded = false;
            const time_t nowEpoch = time(nullptr);
            hb.message.payload.heartbeat.epoch_s =
                (nowEpoch >= 1600000000) ? (uint32_t)nowEpoch : 0;
            strncpy(hb.message.payload.heartbeat.device_id, _deviceId,
                    sizeof(hb.message.payload.heartbeat.device_id) - 1);
            if (xQueueSend(_serialTxQueue, &hb, 0) != pdPASS) {
                ESP_LOGW(TAG, "TX queue piena: heartbeat verso il Mega saltato");
            }
        }

        // Telemetry fallback: if the Mega is connected and we haven't published recently
        if (_isMegaConnected && (millis() - _lastTelemetryPublishMs >= TELEMETRY_PUBLISH_INTERVAL_MS)) {
            TelemetryFast tf;
            TelemetryDeep td;
            getTelemetrySnapshot(tf, td);
            publishTelemetryJson(tf, td);
            _lastTelemetryPublishMs = millis();
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

    // Anti-loopback: the subscription is command/#, which also re-captures our
    // own messages on command/ack (and the retained /status). Ignore them.
    if (strstr(cmd.topic, "/command/ack") != nullptr ||
        strstr(cmd.topic, "/status")      != nullptr) {
        return;
    }

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

    // Topic/action consistency: if the last topic segment is a known action
    // different from the payload's "type", reject it (avoids commands
    // disguised on a topic that doesn't match the requested action).
    const char* seg = strrchr(cmd.topic, '/');
    if (seg) {
        seg++; // skip the slash
        bool segIsAction =
            !strcmp(seg, "setMode") || !strcmp(seg, "water") || !strcmp(seg, "stop") ||
            !strcmp(seg, "requestDiagnostics") || !strcmp(seg, "setMotionParams") ||
            !strcmp(seg, "readSoil") || !strcmp(seg, "softReset");
        if (segIsAction && strcmp(seg, type) != 0) {
            ESP_LOGW(TAG, "Topic/type mismatch: topic=%s type=%s", cmd.topic, type);
            publishAlarmJson("topic_type_mismatch", type);
            return;
        }
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
        // Defense-in-depth: cap the duration to 30 s on the Hub (on top of the
        // Mega's own limits) to protect the plant and pump from anomalous commands.
        uint32_t dur = doc["duration_ms"] | 0;
        if (dur > 30000) {
            ESP_LOGW(TAG, "Watering %lu ms troncato a 30000 ms (safety)", (unsigned long)dur);
            dur = 30000;
        }
        megaCmd.command_type.water.duration_ms = dur;
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
            portENTER_CRITICAL(&_telemetryMux);
            _lastFastTelemetry = msg.payload.telemetry_fast;
            portEXIT_CRITICAL(&_telemetryMux);
            break;
        case WrapperMessage_telemetry_deep_tag:
            portENTER_CRITICAL(&_telemetryMux);
            _lastDeepTelemetry = msg.payload.telemetry_deep;
            portEXIT_CRITICAL(&_telemetryMux);
            publishTelemetryJson(_lastFastTelemetry, _lastDeepTelemetry);
            _lastTelemetryPublishMs = millis();
            break;
        case WrapperMessage_log_tag:
            ESP_LOGI(TAG, "Mega log [%s] %s: %s", logLevelToStr(msg.payload.log.level),
                     msg.payload.log.event, msg.payload.log.detail);
            publishLogJson(msg.payload.log);
            break;
        case WrapperMessage_heartbeat_tag:
            // Timestamp update only (already done above). Nothing to publish.
            break;
        case WrapperMessage_command_response_tag:
            // Serial echo: in a lab setup without MQTT, this is the only
            // visible feedback for the outcome of a command sent to the Mega.
            Serial.printf("[ACK Mega] cmd_id=%lu status=%s detail=%s value=%ld exec=%lu ms\n",
                          (unsigned long)msg.payload.command_response.cmd_id,
                          msg.payload.command_response.status == CommandResponse_Status_OK ? "OK" : "ERROR",
                          msg.payload.command_response.detail,
                          (long)msg.payload.command_response.value,
                          (unsigned long)msg.payload.command_response.exec_time_ms);
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

static const char* careStateToStr(uint32_t s) {
    // Codes mirror the CareState enum in the Mega's CarePolicy.h; kept as a
    // plain uint32 on the wire (no shared enum) so a v4.0 decoder simply
    // ignores the field.
    switch (s) {
        case 1:  return "SEEK_SUN";
        case 2:  return "BASK";
        case 3:  return "SEEK_SHADE";
        case 4:  return "SHELTER";
        case 5:  return "TOP_UP";
        case 0:
        default: return "NIGHT";
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
    const uint32_t epochS   = tf.epoch_s != 0 ? tf.epoch_s : td.epoch_s;
    doc["timestamp_utc"]    = epochS;
    // Plausibility flag for consumers: with the Mega's software fallback clock
    // (RTC absent/unset) epochs restart from 1970-01-01 08:00, so historical
    // data would be mis-dated. Anything before 2020-09-13 (1.6e9) is not real.
    doc["time_valid"]       = epochS >= 1600000000UL;
    doc["uptime_s"]         = td.uptime_s;
    doc["device_id"]        = _deviceId;
    doc["fw_version"]       = HUB_FW_VERSION;
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

    // Ambient fields: included only if actually measured. The Mega sends NaN
    // when the BME680 is not fitted, and likewise for battery when monitoring
    // is off; publishing them as 0 would mislead the app into treating them
    // as real readings.
    if (!isnan(td.temperature_c))    doc["temperature_c"]       = td.temperature_c;
    if (!isnan(td.humidity_percent)) doc["humidity_percent"]    = td.humidity_percent;
    if (!isnan(td.pressure_hpa))     doc["pressure_hpa"]        = td.pressure_hpa;
    if (td.gas_resistance_ohms > 0)  doc["gas_resistance_ohms"] = td.gas_resistance_ohms;
    if (!isnan(td.battery_voltage) && td.battery_voltage > 0.0f)
        doc["battery_voltage"] = td.battery_voltage;
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
    c["light_seeking_sessions"]       = td.light_seeking_sessions;
    c["shadow_seeking_sessions"]      = td.shadow_seeking_sessions;
    c["escape_attempts"]              = td.escape_attempts;

    // Autonomous-care daily KPIs (proto v4.1, Mega v5.3+). Always published,
    // with the enabled flag, so the app can render the "plant wellness"
    // summary and distinguish "care off" from "care idle".
    JsonObject care = doc.createNestedObject("care");
    care["enabled"]            = td.care_enabled;
    care["state"]              = careStateToStr(td.care_state);
    care["light_budget_pct"]   = td.light_budget_pct;
    care["relocations_today"]  = td.relocations_today;
    care["water_doses_today"]  = td.water_doses_today;
    care["growlight_min_today"] = td.growlight_minutes_today;

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

void MainLogic::getTelemetrySnapshot(TelemetryFast& tf, TelemetryDeep& td) {
    portENTER_CRITICAL(&_telemetryMux);
    tf = _lastFastTelemetry;
    td = _lastDeepTelemetry;
    portEXIT_CRITICAL(&_telemetryMux);
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
