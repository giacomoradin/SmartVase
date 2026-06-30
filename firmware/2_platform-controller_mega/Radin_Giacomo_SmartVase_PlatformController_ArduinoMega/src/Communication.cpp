/*!
    @file   Communication.cpp

    @ingroup MegaComm

    @brief  Implementazione di Communication: framing seriale, coda log ed esecuzione comandi.

    @date   2026-04-29

    @author Giacomo Radin
*/

#include "Communication.h"
#include "Movement.h"
#include "Persistence.h"
#include "Sensors.h"
#include "Pump.h"
#include "SystemStatus.h"
#include "Crc16.h"
#include "CommandPolicy.h"

// =================================================================
// Framing: SOF | len_hi | len_lo | payload | crc16_hi | crc16_lo
// =================================================================
#define SOF_BYTE 0xAA

// Coda log circolare.
#define LOG_QUEUE_SIZE 20
struct LogEntry {
    Log_LogLevel level;
    char         event[24];
    char         detail[32];
    uint32_t     timestamp_ms;
};
static LogEntry logQueue[LOG_QUEUE_SIZE];
static int logQueueHead  = 0;
static int logQueueTail  = 0;
static int logQueueCount = 0;

// Intervallo minimo fra due irrigazioni accettate: protegge la pianta dall'
// over-watering e il sistema da flood di WaterCommand (oltre al cap 60 s della
// pompa e al rifiuto-se-gia'-attiva). Tarabile in base alla pianta reale.
#define WATER_MIN_INTERVAL_MS 5000UL
// Limiti di sicurezza validati anche lato Mega (defense-in-depth, non solo Hub).
#define WATER_MAX_DURATION_MS 30000UL  // cap durata pompa (coerente con l'Hub)
#define MOTION_PARAM_MIN_MS   100      // range avoidance reverse/turn
#define MOTION_PARAM_MAX_MS   5000

Communication::Communication() : last_hub_message_ms(0), last_water_ms(0), _lastExecutedCmdId(0) {
    memset(protobuf_tx_buffer, 0, sizeof(protobuf_tx_buffer));
    memset(protobuf_rx_buffer, 0, sizeof(protobuf_rx_buffer));
}

void Communication::init() {
    // Serial1 e' inizializzata dal main: questa funzione esiste come hook
    // per eventuali setup futuri (timer ack, ecc.).
    last_hub_message_ms = millis();
}

void Communication::sendFramedMessage(const uint8_t* payload, uint16_t len) {
    if (len == 0) return;
    uint16_t crc = crc16_ccitt(payload, len);
    Serial1.write(SOF_BYTE);
    Serial1.write((uint8_t)(len >> 8));
    Serial1.write((uint8_t)(len & 0xFF));
    Serial1.write(payload, len);
    Serial1.write((uint8_t)(crc >> 8));
    Serial1.write((uint8_t)(crc & 0xFF));
}

void Communication::sendProtobufMessage(const WrapperMessage& message) {
    pb_ostream_t stream = pb_ostream_from_buffer(protobuf_tx_buffer, sizeof(protobuf_tx_buffer));
    if (!pb_encode(&stream, WrapperMessage_fields, &message)) return;
    sendFramedMessage(protobuf_tx_buffer, (uint16_t)stream.bytes_written);
}

void Communication::sendFastTelemetry(const TelemetryFast& tf) {
    WrapperMessage m = WrapperMessage_init_zero;
    m.which_payload = WrapperMessage_telemetry_fast_tag;
    m.payload.telemetry_fast = tf;
    sendProtobufMessage(m);
}

void Communication::sendDeepTelemetry(const TelemetryDeep& td) {
    WrapperMessage m = WrapperMessage_init_zero;
    m.which_payload = WrapperMessage_telemetry_deep_tag;
    m.payload.telemetry_deep = td;
    sendProtobufMessage(m);
}

void Communication::sendHeartbeat(uint32_t uptime_s, bool isDegraded, const char* deviceId) {
    WrapperMessage m = WrapperMessage_init_zero;
    m.which_payload = WrapperMessage_heartbeat_tag;
    Heartbeat& hb = m.payload.heartbeat;
    hb.uptime_s    = uptime_s;
    hb.is_degraded = isDegraded;
    if (deviceId) {
        strncpy(hb.device_id, deviceId, sizeof(hb.device_id) - 1);
        hb.device_id[sizeof(hb.device_id) - 1] = '\0';
    }
    sendProtobufMessage(m);
}

void Communication::sendCommandResponse(CommandResponse_Status status, const char* detail,
                                        int32_t value, uint32_t cmd_id, uint32_t exec_time_ms) {
    WrapperMessage m = WrapperMessage_init_zero;
    m.which_payload = WrapperMessage_command_response_tag;
    CommandResponse& r = m.payload.command_response;
    r.status       = status;
    r.value        = value;
    r.cmd_id       = cmd_id;
    r.exec_time_ms = exec_time_ms;
    if (detail) {
        strncpy(r.detail, detail, sizeof(r.detail) - 1);
        r.detail[sizeof(r.detail) - 1] = '\0';
    }
    sendProtobufMessage(m);
}

void Communication::logEvent(Log_LogLevel level, const char* event, const char* detail,
                             const char* /*deviceId*/, CumulativeStats& stats) {
    // ISR-safe enqueue: noInterrupts limita lo scope al solo aggiornamento dei contatori.
    noInterrupts();
    // Backpressure: oltre l'80% della coda scarta gli INFO per lasciare spazio
    // agli eventi importanti (WARN/ERROR/CRITICAL). Non conta come overflow.
    if (logQueueCount >= (LOG_QUEUE_SIZE * 4 / 5) && level == Log_LogLevel_INFO) {
        interrupts();
        return;
    }
    if (logQueueCount >= LOG_QUEUE_SIZE) {
        stats.log_overflows++;
        interrupts();
        return;
    }
    LogEntry& e = logQueue[logQueueTail];
    e.level        = level;
    e.timestamp_ms = millis();
    strncpy(e.event, event ? event : "", sizeof(e.event) - 1);
    e.event[sizeof(e.event) - 1] = '\0';
    strncpy(e.detail, detail ? detail : "", sizeof(e.detail) - 1);
    e.detail[sizeof(e.detail) - 1] = '\0';
    logQueueTail = (logQueueTail + 1) % LOG_QUEUE_SIZE;
    logQueueCount++;
    interrupts();
}

void Communication::drainLogQueue(const char* deviceId) {
    if (logQueueCount == 0) return;
    LogEntry entry;
    noInterrupts();
    entry = logQueue[logQueueHead];
    logQueueHead = (logQueueHead + 1) % LOG_QUEUE_SIZE;
    logQueueCount--;
    interrupts();

    WrapperMessage m = WrapperMessage_init_zero;
    m.which_payload = WrapperMessage_log_tag;
    Log& lg = m.payload.log;
    lg.level        = entry.level;
    lg.timestamp_ms = entry.timestamp_ms;
    strncpy(lg.event,  entry.event,  sizeof(lg.event)  - 1);
    strncpy(lg.detail, entry.detail, sizeof(lg.detail) - 1);
    if (deviceId) {
        strncpy(lg.source_device, deviceId, sizeof(lg.source_device) - 1);
    }
    sendProtobufMessage(m);
}

// =================================================================
// RX: parser a stati per il framing
// =================================================================
void Communication::handleSerial(Movement& movement, Persistence& persistence,
                                 Sensors& sensors, Pump& pump, SystemStatus& sys) {
    static enum {
        WAIT_SOF, WAIT_LEN_H, WAIT_LEN_L, WAIT_PAYLOAD, WAIT_CRC_H, WAIT_CRC_L
    } state = WAIT_SOF;
    static uint16_t len = 0;
    static uint16_t pos = 0;
    static uint16_t received_crc = 0;

    while (Serial1.available()) {
        uint8_t b = Serial1.read();
        switch (state) {
            case WAIT_SOF:
                if (b == SOF_BYTE) {
                    state = WAIT_LEN_H;
                    len = 0;
                    pos = 0;
                    received_crc = 0;
                }
                break;
            case WAIT_LEN_H:
                len = (uint16_t)b << 8;
                state = WAIT_LEN_L;
                break;
            case WAIT_LEN_L:
                len |= b;
                if (len == 0 || len > sizeof(protobuf_rx_buffer)) {
                    persistence.getStats().pb_decode_failures++;
                    state = WAIT_SOF;
                } else {
                    state = WAIT_PAYLOAD;
                }
                break;
            case WAIT_PAYLOAD:
                protobuf_rx_buffer[pos++] = b;
                if (pos == len) state = WAIT_CRC_H;
                break;
            case WAIT_CRC_H:
                received_crc = (uint16_t)b << 8;
                state = WAIT_CRC_L;
                break;
            case WAIT_CRC_L:
                received_crc |= b;
                if (received_crc == crc16_ccitt(protobuf_rx_buffer, len)) {
                    WrapperMessage msg = WrapperMessage_init_zero;
                    pb_istream_t stream = pb_istream_from_buffer(protobuf_rx_buffer, len);
                    if (pb_decode(&stream, WrapperMessage_fields, &msg)) {
                        last_hub_message_ms = millis();
                        executeCommand(msg, movement, persistence, sensors, pump, sys);
                    } else {
                        persistence.getStats().pb_decode_failures++;
                    }
                } else {
                    persistence.getStats().pb_decode_failures++;
                }
                state = WAIT_SOF;
                break;
        }
    }
}

void Communication::executeCommand(const WrapperMessage& message,
                                   Movement& movement, Persistence& persistence,
                                   Sensors& sensors, Pump& pump, SystemStatus& sys) {
    // Solo i WrapperMessage di tipo Command sono comandi attivi.
    if (message.which_payload != WrapperMessage_command_tag) return;

    const Command& cmd = message.payload.command;
    unsigned long t0 = millis();
    uint32_t cmd_id  = cmd.cmd_id;

    CumulativeStats& stats = persistence.getStats();
    DeviceConfig&    cfg   = persistence.getConfig();

    // Idempotenza: un cmd_id gia' eseguito (probabile retry dell'app) viene
    // ri-ACKato SENZA rieseguire l'effetto (es. niente doppia irrigazione).
    // cmd_id == 0 = "senza id": non deduplicato.
    if (cmd_id != 0 && cmd_id == _lastExecutedCmdId) {
        sendCommandResponse(CommandResponse_Status_OK, "duplicate_ignored", 0, cmd_id, 0);
        return;
    }
    if (cmd_id != 0) _lastExecutedCmdId = cmd_id;

    switch (cmd.which_command_type) {
        case Command_water_tag: {
            uint32_t dur = cmd.command_type.water.duration_ms;
            // Defense-in-depth: clamp a max sicuro anche qui (l'Hub gia' limita,
            // ma il Mega non si fida di parametri seriali fuori range).
            uint32_t safeDur = clampWaterDurationMs(dur, WATER_MAX_DURATION_MS);
            if (safeDur != dur) {
                logEvent(Log_LogLevel_WARN, "water_clamped", "over_max", sys.deviceId, stats);
                dur = safeDur;
            }
            if (sys.degradedModeActive) {
                sendCommandResponse(CommandResponse_Status_ERROR, "degraded_mode", 0, cmd_id, millis() - t0);
                break;
            }
            // Anti over-watering / flood: minimo WATER_MIN_INTERVAL_MS fra due
            // irrigazioni accettate (in aggiunta al cap 60 s e al rifiuto se
            // la pompa e' gia' attiva).
            if (!waterAllowed(millis(), last_water_ms, WATER_MIN_INTERVAL_MS)) {
                sendCommandResponse(CommandResponse_Status_ERROR, "water_rate_limited", 0, cmd_id, millis() - t0);
                break;
            }
            // Protezione tanica: niente irrigazione se US4 dice vuota o se
            // la lettura non e' affidabile (fail-safe contro pompa a secco).
            if (sensors.tankLooksEmpty(cfg.tank_empty_cm)) {
                float wl = sensors.getWaterLevel();
                const char* why = isnan(wl) ? "tank_sensor_fault" : "tank_empty";
                logEvent(Log_LogLevel_WARN, "water_blocked", why, sys.deviceId, stats);
                sendCommandResponse(CommandResponse_Status_ERROR, why,
                                    isnan(wl) ? -1 : (int32_t)wl, cmd_id, millis() - t0);
                break;
            }
            if (pump.start(dur, stats)) {
                last_water_ms = millis();
                sendCommandResponse(CommandResponse_Status_OK, "water_started", (int32_t)dur, cmd_id, millis() - t0);
            } else {
                sendCommandResponse(CommandResponse_Status_ERROR, "pump_busy_or_invalid", 0, cmd_id, millis() - t0);
            }
            break;
        }
        case Command_set_mode_tag: {
            auto m = cmd.command_type.set_mode.mode;
            CppMode newMode = CPP_IDLE;
            if      (m == SetModeCommand_Mode_LIGHT)  newMode = CPP_LIGHT;
            else if (m == SetModeCommand_Mode_SHADOW) newMode = CPP_SHADOW;
            movement.setTargetMode(newMode);
            sendCommandResponse(CommandResponse_Status_OK, "mode_set", (int32_t)newMode, cmd_id, millis() - t0);
            break;
        }
        case Command_stop_tag: {
            movement.setTargetMode(CPP_IDLE);
            movement.stopMotors(stats);
            pump.stop(stats);
            sendCommandResponse(CommandResponse_Status_OK, "stopped", 0, cmd_id, millis() - t0);
            break;
        }
        case Command_request_diagnostics_tag: {
            TelemetryDeep td = sensors.buildDeepTelemetry(stats, sys.deviceId);
            sendDeepTelemetry(td);
            sendCommandResponse(CommandResponse_Status_OK, "diag_sent", 0, cmd_id, millis() - t0);
            break;
        }
        case Command_set_motion_params_tag: {
            // Defense-in-depth: clamp dei parametri al range sicuro prima di usarli.
            uint32_t reqRev  = cmd.command_type.set_motion_params.reverse_ms;
            uint32_t reqTurn = cmd.command_type.set_motion_params.turn_ms;
            uint16_t newRev  = clampMotionParamMs(reqRev,  MOTION_PARAM_MIN_MS, MOTION_PARAM_MAX_MS);
            uint16_t newTurn = clampMotionParamMs(reqTurn, MOTION_PARAM_MIN_MS, MOTION_PARAM_MAX_MS);
            if (newRev != reqRev || newTurn != reqTurn) {
                logEvent(Log_LogLevel_WARN, "motion_clamped", "out_of_range", sys.deviceId, stats);
            }
            // No-op se invariato: evita una scrittura EEPROM inutile (anti-usura)
            // a ogni comando identico ripetuto.
            if (!motionParamsChanged(cfg.avoid_reverse_ms, cfg.avoid_turn_ms, newRev, newTurn)) {
                sendCommandResponse(CommandResponse_Status_OK, "motion_params_unchanged", 0, cmd_id, millis() - t0);
                break;
            }
            cfg.avoid_reverse_ms = newRev;
            cfg.avoid_turn_ms    = newTurn;
            // Salvataggio EEPROM DEFERITO: lo esegue il main loop quando la
            // seriale e' a riposo, per non bloccare il loop ~60 ms (rischio
            // overflow RX) durante la ricezione di un frame.
            sys.configSavePending = true;
            sendCommandResponse(CommandResponse_Status_OK, "motion_params_set", 0, cmd_id, millis() - t0);
            break;
        }
        case Command_read_soil_tag: {
            int v = sensors.getSoilMoisture();
            sendCommandResponse(CommandResponse_Status_OK, "soil_moisture", (int32_t)v, cmd_id, millis() - t0);
            break;
        }
        case Command_soft_reset_tag: {
            sendCommandResponse(CommandResponse_Status_OK, "soft_reset_pending", 0, cmd_id, millis() - t0);
            sys.softResetRequested = true;
            break;
        }
        default:
            sendCommandResponse(CommandResponse_Status_ERROR, "unknown_cmd", 0, cmd_id, millis() - t0);
            persistence.getStats().pb_decode_failures++;
            break;
    }
}
