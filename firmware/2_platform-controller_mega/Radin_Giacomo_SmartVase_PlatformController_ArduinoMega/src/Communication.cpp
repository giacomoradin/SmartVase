#include "Communication.h"
#include "Movement.h"
#include "Persistence.h"

#define SOF_BYTE 0xAA
#define PROTOBUF_BUFFER_SIZE 256

// --- Log ---
#define LOG_QUEUE_SIZE 20
struct LogEntry { Log_LogLevel level; char event[24]; char detail[32]; };
LogEntry logQueue[LOG_QUEUE_SIZE];
int logQueueHead = 0, logQueueTail = 0, logQueueCount = 0;

Communication::Communication() {
}

void Communication::handleSerial(Movement& movement, Persistence& persistence) {
    static enum {
        WAIT_SOF, WAIT_LEN_H, WAIT_LEN_L, WAIT_PAYLOAD, WAIT_CRC_H, WAIT_CRC_L
    } state = WAIT_SOF;
    static uint16_t len = 0, pos = 0;
    static uint16_t received_crc = 0;

    while (Serial1.available()) {
        uint8_t b = Serial1.read();
        switch (state) {
            case WAIT_SOF:
                if (b == SOF_BYTE) state = WAIT_LEN_H;
                break;
            case WAIT_LEN_H:
                len = (uint16_t)b << 8;
                state = WAIT_LEN_L;
                break;
            case WAIT_LEN_L:
                len |= b;
                if (len > sizeof(protobuf_rx_buffer)) {
                    // LOG_ERROR("frame_error", "Payload too large");
                    // stats.pb_decode_failures++;
                    state = WAIT_SOF;
                } else {
                    pos = 0;
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
                if (received_crc == crc16(protobuf_rx_buffer, len)) {
                    WrapperMessage message = WrapperMessage_init_zero;
                    pb_istream_t stream = pb_istream_from_buffer(protobuf_rx_buffer, len);
                    if (pb_decode(&stream, WrapperMessage_fields, &message)) {
                        executeCommand(message, movement, persistence);
                    } else {
                        // stats.pb_decode_failures++;
                        // LOG_ERROR("pb_decode_failed", stream.errmsg);
                    }
                } else {
                    // stats.pb_decode_failures++;
                    // LOG_ERROR("crc_error", "CRC mismatch");
                }
                state = WAIT_SOF;
                break;
        }
    }
}

void Communication::sendProtobufMessage(const WrapperMessage& message) {
    pb_ostream_t stream = pb_ostream_from_buffer(protobuf_tx_buffer, sizeof(protobuf_tx_buffer));
    if (!pb_encode(&stream, WrapperMessage_fields, &message)) {
        // LOG_ERROR("pb_encode_failed", "Buffer TX too small?");
        return;
    }
    sendFramedMessage(protobuf_tx_buffer, (uint16_t)stream.bytes_written);
}

void Communication::logEvent(Log_LogLevel level, const char* event, const char* detail) {
    noInterrupts();
    if (logQueueCount >= LOG_QUEUE_SIZE) {
        // if (!systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = true; stats.log_overflows++; }
        interrupts(); return;
    }
    LogEntry& entry = logQueue[logQueueTail];
    entry.level = level;
    strncpy(entry.event, event, sizeof(entry.event) - 1);
    entry.event[sizeof(entry.event) - 1] = '\0';
    if (detail) {
        strncpy(entry.detail, detail, sizeof(entry.detail) - 1);
        entry.detail[sizeof(entry.detail) - 1] = '\0';
    } else { entry.detail[0] = '\0'; }
    logQueueTail = (logQueueTail + 1) % LOG_QUEUE_SIZE;
    logQueueCount++;
    interrupts();
}

void Communication::sendLog() {
    if (logQueueCount == 0) {
        // if (systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = false; LOG_INFO("log_queue_ok", NULL); }
        return;
    }
    noInterrupts();
    LogEntry& entry = logQueue[logQueueHead];
    logQueueHead = (logQueueHead + 1) % LOG_QUEUE_SIZE;
    logQueueCount--;
    interrupts();
    
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_log_tag;
    Log& log_payload = message.payload.log;
    log_payload.level = entry.level;
    strncpy(log_payload.event, entry.event, sizeof(log_payload.event) - 1);
    strncpy(log_payload.detail, entry.detail, sizeof(log_payload.detail) - 1);
    // strncpy(log_payload.source_device, DEVICE_ID, sizeof(log_payload.source_device) - 1);
    log_payload.timestamp_ms = millis();
    sendProtobufMessage(message);
}

void Communication::sendHeartbeat() {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_heartbeat_tag;
    Heartbeat& hb = message.payload.heartbeat;
    hb.uptime_s = millis()/1000;
    // hb.is_degraded = systemStatus.degradedModeActive;
    // strncpy(hb.device_id, DEVICE_ID, sizeof(hb.device_id) - 1);
    sendProtobufMessage(message);
}

void Communication::sendFastTelemetry(const TelemetryFast& tf) {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_telemetry_fast_tag;
    message.payload.telemetry_fast = tf;
    sendProtobufMessage(message);
}

void Communication::sendDeepTelemetry(const TelemetryDeep& td) {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_telemetry_deep_tag;
    message.payload.telemetry_deep = td;
    sendProtobufMessage(message);
}

void Communication::sendCommandResponse(CommandResponse_Status status, const char* detail, uint32_t cmd_id, uint32_t exec_time) {
    WrapperMessage responseMsg = WrapperMessage_init_zero;
    responseMsg.which_payload = WrapperMessage_command_response_tag;
    CommandResponse *response = &responseMsg.payload.command_response;
    
    response->status = status;
    strncpy(response->detail, detail, sizeof(response->detail) - 1);
    response->cmd_id = cmd_id;
    response->exec_time_ms = exec_time;

    sendProtobufMessage(responseMsg);
}

void Communication::sendFramedMessage(const uint8_t* payload, uint16_t len) {
    if (len == 0) return;
    uint16_t crc = crc16(payload, len);
    Serial1.write(SOF_BYTE);
    Serial1.write((uint8_t)(len >> 8));
    Serial1.write((uint8_t)(len & 0xFF));
    Serial1.write(payload, len);
    Serial1.write((uint8_t)(crc >> 8));
    Serial1.write((uint8_t)(crc & 0xFF));
}

void Communication::executeCommand(const WrapperMessage& message, Movement& movement, Persistence& persistence) {
    // ... implementation from .ino file ...
}

uint16_t Communication::crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0;
    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
