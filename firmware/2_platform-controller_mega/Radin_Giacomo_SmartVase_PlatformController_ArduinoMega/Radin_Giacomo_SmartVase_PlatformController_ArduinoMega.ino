/*
 * =================================================================
 * SmartVase - Platform Controller (Arduino Mega)
 * Versione: 3.7 (Enterprise Ready - Library Path Fix)
 * =================================================================
 * Autore Originale: Giacomo Radin
 *
 * NOTE DI VERSIONE (v3.7):
 * - ISTRUZIONI FINALI PER LA COMPILAZIONE:
 * 1. Copiare i 7 file core di nanopb (pb.c, pb.h, etc.) in questa cartella.
 * 2. Generare i file smartvase.pb.c e smartvase.pb.h da smartvase_v3.proto.
 * 3. MODIFICARE MANUALMENTE il file generato 'smartvase.pb.h':
 * assicurarsi che la riga sia ESATTAMENTE '#include "pb.h"'.
 * 4. Questo codice compilerà correttamente.
 */

// =================================================================
// 1. DEFINIZIONI PROTOBUF E ALIAS (ORDINE CRITICO)
// =================================================================
// NOTA: Assicurati di seguire le istruzioni nelle note di versione.
extern "C" {
  #include "pb_encode.h"
  #include "pb_decode.h"
  #include "smartvase.pb.h"
}
#include "smartvase_aliases.h"

// =================================================================
// 2. LIBRERIE STANDARD ARDUINO
// =================================================================
#include <Adafruit_BME680.h>
#include <HCSR04.h>
#include <EEPROM.h>
#include <DS3232RTC.h>
#include <TimeLib.h>
#include <avr/wdt.h>
#include <limits.h>
#include <math.h>

// =================================================================
// 3. CONFIGURATION & DEFINITIONS
// =================================================================

#define DEVICE_ID "MEGA_01"
#define SOF_BYTE 0xAA
#define PROTOBUF_BUFFER_SIZE 256

#define EEPROM_MAGIC_NUMBER_STATS  0x18BEEF18
#define EEPROM_MAGIC_NUMBER_CONFIG 0xCF6BEEF6
#define EEPROM_CONFIG_SLOT_0_ADDR 0
#define EEPROM_CONFIG_SLOT_1_ADDR 50
#define EEPROM_STATS_SLOT_0_ADDR 100
#define EEPROM_STATS_SLOT_1_ADDR 200
#define EEPROM_STATS_WRITE_INTERVAL 300000
#define EEPROM_CONFIG_WRITE_INTERVAL 60000

// --- Pinout ---
const int pumpPin = 2, uvaPin = 3, forkPin = A1, photoresistorPin = A0, forkPowerPin = 51;
const int batteryPin = A2; // NUOVO PIN per monitoraggio batteria
const int enA = 7, in1 = 43, in2 = 45;
const int enB = 6, in3 = 47, in4 = 49;
#define TRIG_PIN_WATER 12
#define ECHO_PIN_WATER 8
#define TRIG_PIN_FRONT 38
#define ECHO_PIN_FRONT 39
#define TRIG_PIN_LEFT 13
#define ECHO_PIN_LEFT 11
#define TRIG_PIN_RIGHT 9
#define ECHO_PIN_RIGHT 10

// --- Costanti Operative ---
#define LOW_MEMORY_THRESHOLD_BYTES 800
#define HUB_DEADMAN_TIMEOUT_MS 120000

// --- Costanti per Monitoraggio Batteria (da calibrare) ---
#define ADC_REFERENCE_VOLTAGE 5.0
#define VOLTAGE_DIVIDER_R1 30000.0 // Valore R1 del partitore
#define VOLTAGE_DIVIDER_R2 7500.0  // Valore R2 del partitore

// --- Costanti di Sicurezza e Validazione ---
#define MIN_AVOID_REVERSE_MS 200
#define MAX_AVOID_REVERSE_MS 3000
#define MIN_AVOID_TURN_MS 200
#define MAX_AVOID_TURN_MS 3000

// ... Altre costanti ...
#define SENSOR_INVALID_STREAK_LIMIT 10
#define LOG_QUEUE_SIZE 20
#define MOTOR_SAFETY_TIMEOUT_MS 20000
#define OBSTACLE_DISTANCE_CM 20
#define WATER_TANK_EMPTY_CM 25
#define MAX_PUMP_DURATION_MS 30000
#define COMMAND_DEBOUNCE_MS 5000
#define STUCK_DETECTION_THRESHOLD 3
#define STUCK_COOLDOWN_MS 30000
#define STUCK_BACKOFF_INCREMENT_MS 10000
#define MOVEMENT_PROGRESS_THRESHOLD_CM 2.0

#define LOG_INFO(event, detail) logEvent(Log_LogLevel_INFO, event, detail)
#define LOG_WARN(event, detail) logEvent(Log_LogLevel_WARN, event, detail)
#define LOG_ERROR(event, detail) logEvent(Log_LogLevel_ERROR, event, detail)
#define LOG_CRITICAL(event, detail) logEvent(Log_LogLevel_CRITICAL, event, detail)

// =================================================================
// 4. STRUCTURES, ENUMS & OBJECTS
// =================================================================
Adafruit_BME680 bme;
DS3232RTC myRTC;
UltraSonicDistanceSensor waterSensor(TRIG_PIN_WATER, ECHO_PIN_WATER);
UltraSonicDistanceSensor frontSensor(TRIG_PIN_FRONT, ECHO_PIN_FRONT);
UltraSonicDistanceSensor leftSensor(TRIG_PIN_LEFT, ECHO_PIN_LEFT);
UltraSonicDistanceSensor rightSensor(TRIG_PIN_RIGHT, ECHO_PIN_RIGHT);

struct SystemStatus {
    bool bmeSensorError; bool lowMemoryDetected; bool logQueueOverflow; bool degradedModeActive; bool hubIsMissing; char degradedReason[32];
};
SystemStatus systemStatus = {false, false, false, false, true, ""};

using CppMovementState = MovementState;
using CppMode = SetModeCommand_Mode;

struct DeviceConfig {
    uint32_t magic_number; uint16_t crc16; int motorCalibLeft; int motorCalibRight; uint16_t avoid_reverse_ms; uint16_t avoid_turn_ms;
};
DeviceConfig config;

struct CumulativeStats {
    uint32_t magic_number; uint16_t crc16; uint32_t watchdog_resets; uint32_t total_irrigations; uint32_t obstacles_avoided;
    uint32_t bme_read_errors; uint32_t low_memory_events; uint32_t log_overflows; uint32_t total_irrigation_duration_s;
    uint32_t total_motor_active_time_s; uint32_t pb_decode_failures; uint32_t stuck_events; uint32_t escape_attempts;
    uint32_t no_progress_events;
    uint16_t task_miss_fast_telemetry; uint16_t task_miss_deep_telemetry; uint16_t task_miss_sendlog;
    uint16_t task_miss_heartbeat;
};
CumulativeStats stats, lastSavedStats;

struct LogEntry { Log_LogLevel level; char event[24]; char detail[32]; };
LogEntry logQueue[LOG_QUEUE_SIZE];

void sendFastTelemetry(); void sendDeepTelemetry(); void sendLog(); void sendHeartbeat(); void saveStats(bool force = false);
void stopMotors(); // Forward declaration

struct Task { const char* name; void (*func)(); unsigned long interval; unsigned long lastRun; };
Task tasks[] = {
    { "fast_telemetry", sendFastTelemetry, 5000, 0 },
    { "deep_telemetry", sendDeepTelemetry, 60000, 0 },
    { "send_log", sendLog, 500, 0 },
    { "heartbeat", sendHeartbeat, 15000, 0 }
};
const int numTasks = sizeof(tasks) / sizeof(Task);

// =================================================================
// 5. GLOBAL VARIABLES
// =================================================================
uint8_t protobuf_tx_buffer[PROTOBUF_BUFFER_SIZE];
uint8_t protobuf_rx_buffer[PROTOBUF_BUFFER_SIZE];
uint8_t mcusr_mirror __attribute__ ((section (".noinit")));

float cached_battery_voltage = NAN;
char cliBuffer[64];
uint8_t cliBufferPos = 0;

CppMovementState currentMovementState = M_IDLE;
CppMode targetMode = IDLE;
unsigned long lastHubMessageTime = 0;
unsigned long motorActiveStartTime = 0;
unsigned long pumpStartTime = 0, pumpDuration = 0;
bool isPumping = false;
bool soilReadingRequested = false;
unsigned long soilReadingStartTime = 0;
unsigned long lastWaterCommandTime = 0;
float cached_water_level_cm = NAN, cached_front_dist_cm = NAN, cached_left_dist_cm = NAN, cached_right_dist_cm = NAN;
unsigned int invalid_streak_water = 0, invalid_streak_front = 0, invalid_streak_left = 0, invalid_streak_right = 0;
int cached_lux = -1;
int logQueueHead = 0, logQueueTail = 0, logQueueCount = 0;
unsigned long lastEepromConfigWrite = 0, lastEepromStatsWrite = 0;
unsigned long stateStartTime = 0;
bool testMode = false;
unsigned long lastFrontSampleTime = 0, lastSideSampleTime = 0, lastWaterSampleTime = 0, lastLightSampleTime = 0;
uint8_t avoidance_attempts = 0;
unsigned long stuck_cooldown_start_time = 0;
uint32_t current_stuck_backoff = STUCK_COOLDOWN_MS;
float pre_maneuver_dist = 0;
int lightThreshold = 600;

// =================================================================
// 6. UTILITY & DIAGNOSTIC FUNCTIONS
// =================================================================
int freeRam() { extern int __heap_start, *__brkval; int v; return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); }
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) { mcusr_mirror = MCUSR; MCUSR = 0; wdt_disable(); }
time_t getRTCtime() { return myRTC.get(); }

uint16_t crc16(const uint8_t* data, size_t length) {
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

void enterDegradedMode(const char* reason) {
    if (systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = true;
    strncpy(systemStatus.degradedReason, reason, sizeof(systemStatus.degradedReason) - 1);
    systemStatus.degradedReason[sizeof(systemStatus.degradedReason)-1] = '\0';
    stopMotors();
    currentMovementState = M_IDLE;
    LOG_CRITICAL("degraded_mode", reason);
    saveStats(true);
}
void exitDegradedMode() {
    if (!systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = false;
    systemStatus.degradedReason[0] = '\0';
    LOG_INFO("degraded_exit", "Exiting degraded mode");
}

void softReset() {
    LOG_WARN("soft_reset", "Initiating software reset");
    delay(100);
    wdt_enable(WDTO_15MS);
    while (true);
}

// =================================================================
// 7. LOGGING & COMMUNICATION FUNCTIONS
// =================================================================
void logEvent(Log_LogLevel level, const char* event, const char* detail) {
    noInterrupts();
    if (logQueueCount >= LOG_QUEUE_SIZE) {
        if (!systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = true; stats.log_overflows++; }
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

void sendFramedMessage(const uint8_t* payload, uint16_t len) {
    if (len == 0) return;
    uint16_t crc = crc16(payload, len);
    Serial1.write(SOF_BYTE);
    Serial1.write((uint8_t)(len >> 8));
    Serial1.write((uint8_t)(len & 0xFF));
    Serial1.write(payload, len);
    Serial1.write((uint8_t)(crc >> 8));
    Serial1.write((uint8_t)(crc & 0xFF));
}

void sendProtobufMessage(const WrapperMessage& message) {
    pb_ostream_t stream = pb_ostream_from_buffer(protobuf_tx_buffer, sizeof(protobuf_tx_buffer));
    if (!pb_encode(&stream, WrapperMessage_fields, &message)) {
        LOG_ERROR("pb_encode_failed", "Buffer TX too small?");
        return;
    }
    sendFramedMessage(protobuf_tx_buffer, (uint16_t)stream.bytes_written);
}

void sendCommandResponse(CommandResponse_Status status, const char* detail, uint32_t cmd_id, uint32_t exec_time) {
    WrapperMessage responseMsg = WrapperMessage_init_zero;
    responseMsg.which_payload = WrapperMessage_command_response_tag;
    CommandResponse *response = &responseMsg.payload.command_response;
    
    response->status = status;
    strncpy(response->detail, detail, sizeof(response->detail) - 1);
    response->cmd_id = cmd_id;
    response->exec_time_ms = exec_time;

    sendProtobufMessage(responseMsg);
}

void executeCommand(const WrapperMessage& message) {
    lastHubMessageTime = millis();
    if (systemStatus.hubIsMissing) {
        systemStatus.hubIsMissing = false;
        LOG_INFO("hub_recovered", "Communication restored");
        if (strcmp(systemStatus.degradedReason, "Hub Missing") == 0) {
            exitDegradedMode();
        }
    }

    if (message.which_payload != WrapperMessage_command_tag) { 
      sendCommandResponse(CommandResponse_Status_ERROR, "Not a command", 0, 0); 
      return; 
    }
    
    const Command& cmd = message.payload.command;
    uint32_t cmd_id = cmd.cmd_id;
    unsigned long startTime = millis();
    
    switch (cmd.which_command_type) {
        case Command_water_tag: {
            if (isPumping) { sendCommandResponse(CommandResponse_Status_ERROR, "Pump already active", cmd_id, millis() - startTime); return; }
            if (cached_water_level_cm > WATER_TANK_EMPTY_CM) { sendCommandResponse(CommandResponse_Status_ERROR, "Water tank empty", cmd_id, millis() - startTime); return; }
            if (millis() - lastWaterCommandTime < COMMAND_DEBOUNCE_MS) { sendCommandResponse(CommandResponse_Status_ERROR, "Debounce active", cmd_id, millis() - startTime); return; }
            
            unsigned long duration = cmd.command_type.water.duration_ms;
            if (duration == 0 || duration > MAX_PUMP_DURATION_MS) { sendCommandResponse(CommandResponse_Status_ERROR, "Invalid duration", cmd_id, millis() - startTime); return; }
            
            isPumping = true;
            pumpStartTime = millis();
            pumpDuration = duration;
            if (!testMode) digitalWrite(pumpPin, HIGH);
            stats.total_irrigations++;
            lastWaterCommandTime = millis();
            sendCommandResponse(CommandResponse_Status_OK, "Irrigation started", cmd_id, millis() - startTime);
            break;
        }
        case Command_set_mode_tag: {
            targetMode = (CppMode)cmd.command_type.set_mode.mode;
            sendCommandResponse(CommandResponse_Status_OK, "Mode updated", cmd_id, millis() - startTime);
            break;
        }
        case Command_stop_tag: {
            targetMode = IDLE;
            currentMovementState = M_IDLE;
            stopMotors();
            sendCommandResponse(CommandResponse_Status_OK, "All systems stopped", cmd_id, millis() - startTime);
            break;
        }
        case Command_request_diagnostics_tag: {
            sendDeepTelemetry();
            sendCommandResponse(CommandResponse_Status_OK, "Deep telemetry sent", cmd_id, millis() - startTime);
            break;
        }
        
        case Command_set_motion_params_tag: {
            uint32_t reverse_ms = cmd.command_type.set_motion_params.reverse_ms;
            uint32_t turn_ms = cmd.command_type.set_motion_params.turn_ms;
            
            if (reverse_ms >= MIN_AVOID_REVERSE_MS && reverse_ms <= MAX_AVOID_REVERSE_MS &&
                turn_ms >= MIN_AVOID_TURN_MS && turn_ms <= MAX_AVOID_TURN_MS) {
                
                config.avoid_reverse_ms = reverse_ms;
                config.avoid_turn_ms = turn_ms;
                saveConfig();
                sendCommandResponse(CommandResponse_Status_OK, "Motion params updated", cmd_id, millis() - startTime);
            } else { 
                sendCommandResponse(CommandResponse_Status_ERROR, "Unsafe parameter range", cmd_id, millis() - startTime); 
            }
            break;
        }

        case Command_read_soil_tag: {
            if (!soilReadingRequested) {
                digitalWrite(forkPowerPin, HIGH);
                soilReadingStartTime = millis();
                soilReadingRequested = true;
                sendCommandResponse(CommandResponse_Status_OK, "Soil reading initiated", cmd_id, millis() - startTime);
            } else { sendCommandResponse(CommandResponse_Status_ERROR, "Reading already in progress", cmd_id, millis() - startTime); }
            break;
        }
        case Command_soft_reset_tag: {
            sendCommandResponse(CommandResponse_Status_OK, "Soft reset initiated", cmd_id, millis() - startTime);
            softReset();
            break;
        }
        default:
            sendCommandResponse(CommandResponse_Status_ERROR, "Unknown command", cmd_id, millis() - startTime);
            break;
    }
}

void handleSerial() {
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
                    LOG_ERROR("frame_error", "Payload too large");
                    stats.pb_decode_failures++;
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
                        executeCommand(message);
                    } else {
                        stats.pb_decode_failures++;
                        LOG_ERROR("pb_decode_failed", stream.errmsg);
                    }
                } else {
                    stats.pb_decode_failures++;
                    LOG_ERROR("crc_error", "CRC mismatch");
                }
                state = WAIT_SOF;
                break;
        }
    }
}

void sendLog() {
    if (logQueueCount == 0) {
        if (systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = false; LOG_INFO("log_queue_ok", NULL); }
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
    strncpy(log_payload.source_device, DEVICE_ID, sizeof(log_payload.source_device) - 1);
    log_payload.timestamp_ms = millis();
    sendProtobufMessage(message);
}

void sendHeartbeat() {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_heartbeat_tag;
    Heartbeat& hb = message.payload.heartbeat;
    hb.uptime_s = millis()/1000;
    hb.is_degraded = systemStatus.degradedModeActive;
    strncpy(hb.device_id, DEVICE_ID, sizeof(hb.device_id) - 1);
    sendProtobufMessage(message);
}

void sendFastTelemetry() {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_telemetry_fast_tag;
    TelemetryFast& tf = message.payload.telemetry_fast;
    
    if (!isnan(cached_front_dist_cm)) tf.front_dist_cm = cached_front_dist_cm;
    if (!isnan(cached_left_dist_cm)) tf.left_dist_cm = cached_left_dist_cm;
    if (!isnan(cached_right_dist_cm)) tf.right_dist_cm = cached_right_dist_cm;
    if (!isnan(cached_water_level_cm)) tf.water_level_cm = cached_water_level_cm;
    if (cached_lux != -1) tf.lux = cached_lux;
    tf.movement_state = (MovementState)currentMovementState;
    strncpy(tf.device_id, DEVICE_ID, sizeof(tf.device_id) - 1);
    
    sendProtobufMessage(message);
}

void sendDeepTelemetry() {
    WrapperMessage message = WrapperMessage_init_zero;
    message.which_payload = WrapperMessage_telemetry_deep_tag;
    TelemetryDeep& td = message.payload.telemetry_deep;

    if (bme.performReading()) {
        if (systemStatus.bmeSensorError) { systemStatus.bmeSensorError = false; LOG_INFO("sensor_ok", "BME680"); }
        td.temperature_c = bme.temperature;
        td.humidity_percent = bme.humidity;
        td.pressure_hpa = bme.pressure / 100.0F;
        td.gas_resistance_ohms = bme.gas_resistance;
    } else {
        stats.bme_read_errors++;
        if (!systemStatus.bmeSensorError) { systemStatus.bmeSensorError = true; LOG_WARN("sensor_fail", "BME680"); }
    }
    
    td.uptime_s = millis() / 1000;
    td.free_ram_bytes = freeRam();
    td.watchdog_resets = stats.watchdog_resets;
    td.total_irrigations = stats.total_irrigations;
    td.obstacles_avoided = stats.obstacles_avoided;
    td.stuck_events = stats.stuck_events;
    td.bme_read_errors = stats.bme_read_errors;
    td.log_overflows = stats.log_overflows;
    td.total_irrigation_duration_s = stats.total_irrigation_duration_s;
    td.total_motor_active_time_s = stats.total_motor_active_time_s;
    td.pb_decode_failures = stats.pb_decode_failures;
    strncpy(td.device_id, DEVICE_ID, sizeof(td.device_id) - 1);
    
    if (!isnan(cached_battery_voltage)) {
        td.battery_voltage = cached_battery_voltage;
    }

    sendProtobufMessage(message);
}

// =================================================================
// 8. SENSOR & CORE LOGIC FUNCTIONS
// =================================================================

float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak, float min_valid, float max_valid) {
    const float alpha = 0.4;
    if (isnan(raw_value) || raw_value < min_valid || raw_value > max_valid) {
        invalid_streak++;
        return (invalid_streak >= SENSOR_INVALID_STREAK_LIMIT) ? NAN : last_value;
    }
    invalid_streak = 0;
    if (isnan(last_value)) return raw_value;
    return (alpha * raw_value) + ((1.0 - alpha) * last_value);
}

void sampleSensors() {
    unsigned long currentMillis = millis();
    if (currentMillis - lastFrontSampleTime >= 100) {
        float raw = frontSensor.measureDistanceCm();
        cached_front_dist_cm = applyEmaFilter(raw, cached_front_dist_cm, invalid_streak_front, 2.0, 400.0);
        lastFrontSampleTime = currentMillis;
    }
    if (currentMillis - lastSideSampleTime >= 200) {
        float raw_l = leftSensor.measureDistanceCm();
        cached_left_dist_cm = applyEmaFilter(raw_l, cached_left_dist_cm, invalid_streak_left, 2.0, 400.0);
        float raw_r = rightSensor.measureDistanceCm();
        cached_right_dist_cm = applyEmaFilter(raw_r, cached_right_dist_cm, invalid_streak_right, 2.0, 400.0);
        lastSideSampleTime = currentMillis;
    }
    if (currentMillis - lastWaterSampleTime >= 1000) {
        float raw = waterSensor.measureDistanceCm();
        cached_water_level_cm = applyEmaFilter(raw, cached_water_level_cm, invalid_streak_water, 2.0, 400.0);
        lastWaterSampleTime = currentMillis;
    }
    if (currentMillis - lastLightSampleTime >= 500) {
        cached_lux = analogRead(photoresistorPin);
        lastLightSampleTime = currentMillis;
    }

    int rawAdc = analogRead(batteryPin);
    float voltage = (rawAdc / 1023.0) * ADC_REFERENCE_VOLTAGE;
    cached_battery_voltage = voltage * ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2);
}

void handleSoilReading() {
    if (soilReadingRequested && millis() - soilReadingStartTime >= 50) {
        int soilRaw = analogRead(forkPin);
        digitalWrite(forkPowerPin, LOW);
        soilReadingRequested = false;
        char buf[10]; snprintf(buf, sizeof(buf), "%d", soilRaw);
        LOG_INFO("soil_reading", buf);
    }
}

void handlePumping() {
    if (isPumping && millis() - pumpStartTime >= pumpDuration) {
        if (!testMode) digitalWrite(pumpPin, LOW);
        isPumping = false;
        stats.total_irrigation_duration_s += pumpDuration / 1000;
        LOG_INFO("irrigation_end", "Completed");
    }
}

void stopMotors() {
    analogWrite(enA, 0); analogWrite(enB, 0);
    digitalWrite(in1, LOW); digitalWrite(in2, LOW);
    digitalWrite(in3, LOW); digitalWrite(in4, LOW);
    if (motorActiveStartTime > 0) {
        stats.total_motor_active_time_s += (millis() - motorActiveStartTime) / 1000;
        motorActiveStartTime = 0;
    }
}

void moveForward() { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }
void moveBackward() { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void turnRight() { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void turnLeft() { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }

void handleMovementSM() {
    if (systemStatus.degradedModeActive) {
        if (currentMovementState != M_IDLE) { stopMotors(); currentMovementState = M_IDLE; }
        return;
    }
    if (motorActiveStartTime > 0 && millis() - motorActiveStartTime > MOTOR_SAFETY_TIMEOUT_MS) {
        LOG_ERROR("motor_timeout", "Safety stop");
        stopMotors();
        currentMovementState = M_IDLE;
        return;
    }
    bool front_obs = !isnan(cached_front_dist_cm) && cached_front_dist_cm < OBSTACLE_DISTANCE_CM;
    bool left_obs = !isnan(cached_left_dist_cm) && cached_left_dist_cm < OBSTACLE_DISTANCE_CM;
    bool right_obs = !isnan(cached_right_dist_cm) && cached_right_dist_cm < OBSTACLE_DISTANCE_CM;

    switch (currentMovementState) {
        case M_IDLE:
            stopMotors();
            if (targetMode != IDLE) {
                currentMovementState = M_MOVING;
                motorActiveStartTime = millis();
                avoidance_attempts = 0;
                current_stuck_backoff = STUCK_COOLDOWN_MS;
            }
            break;
        case M_MOVING:
            if (front_obs) {
                stats.obstacles_avoided++;
                currentMovementState = M_AVOID_START;
            } else {
                if (targetMode == LIGHT && cached_lux < lightThreshold) turnRight();
                else if (targetMode == SHADOW && cached_lux > lightThreshold) turnLeft();
                else moveForward();
            }
            break;
        case M_AVOID_START:
            stopMotors();
            pre_maneuver_dist = cached_front_dist_cm;
            stateStartTime = millis();
            currentMovementState = M_AVOID_REVERSING;
            stats.escape_attempts++;
            break;
        case M_AVOID_REVERSING:
            moveBackward();
            if (millis() - stateStartTime > config.avoid_reverse_ms) {
                stateStartTime = millis();
                currentMovementState = M_AVOID_TURNING;
            }
            break;
        case M_AVOID_TURNING:
            if (millis() - stateStartTime > config.avoid_turn_ms) {
                if (!isnan(pre_maneuver_dist) && !isnan(cached_front_dist_cm) && abs(cached_front_dist_cm - pre_maneuver_dist) < MOVEMENT_PROGRESS_THRESHOLD_CM) {
                    stats.no_progress_events++;
                }
                if (front_obs) {
                    avoidance_attempts++;
                    if (avoidance_attempts >= STUCK_DETECTION_THRESHOLD) {
                        stats.stuck_events++;
                        LOG_CRITICAL("stuck_detected", "Entering STUCK state");
                        currentMovementState = M_STUCK;
                        stuck_cooldown_start_time = millis();
                    } else { currentMovementState = M_AVOID_START; }
                } else { currentMovementState = M_MOVING; }
                break;
            }
            if (!right_obs && right_obs != left_obs) turnRight();
            else if (!left_obs) turnLeft();
            else { if (random(0, 2) == 0) turnLeft(); else turnRight(); }
            break;
        case M_STUCK:
            stopMotors();
            if (millis() - stuck_cooldown_start_time > current_stuck_backoff) {
                LOG_INFO("stuck_cooldown_end", "Attempting recovery");
                currentMovementState = M_IDLE;
                current_stuck_backoff += STUCK_BACKOFF_INCREMENT_MS;
            }
            break;
    }
}

// =================================================================
// 9. PERSISTENCE FUNCTIONS
// =================================================================
void loadConfig() {
    DeviceConfig c0, c1;
    EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
    EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
    uint16_t crc0_calc = crc16((uint8_t*)&c0, sizeof(c0) - sizeof(c0.crc16));
    uint16_t crc1_calc = crc16((uint8_t*)&c1, sizeof(c1) - sizeof(c1.crc16));
    if (c0.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c0.crc16 == crc0_calc) { config = c0; LOG_INFO("eeprom_load", "Config slot 0 OK"); }
    else if (c1.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c1.crc16 == crc1_calc) { config = c1; LOG_INFO("eeprom_load", "Config slot 1 OK"); }
    else { config = {EEPROM_MAGIC_NUMBER_CONFIG, 0, 255, 240, 1000, 1200}; LOG_WARN("eeprom_load", "Config invalid, using defaults"); saveConfig(); }
}
void saveConfig() {
    if (millis() - lastEepromConfigWrite < EEPROM_CONFIG_WRITE_INTERVAL) return;
    config.crc16 = crc16((uint8_t*)&config, sizeof(config) - sizeof(config.crc16));
    static uint8_t slot = 0;
    slot = (slot + 1) % 2;
    EEPROM.put(slot == 0 ? EEPROM_CONFIG_SLOT_0_ADDR : EEPROM_CONFIG_SLOT_1_ADDR, config);
    lastEepromConfigWrite = millis();
}
void loadStats() {
    CumulativeStats s0, s1;
    EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
    EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);
    uint16_t crc0_calc = crc16((uint8_t*)&s0, sizeof(s0) - sizeof(s0.crc16));
    uint16_t crc1_calc = crc16((uint8_t*)&s1, sizeof(s1) - sizeof(s1.crc16));
    if (s0.magic_number == EEPROM_MAGIC_NUMBER_STATS && s0.crc16 == crc0_calc) { stats = s0; LOG_INFO("eeprom_load", "Stats slot 0 OK");}
    else if (s1.magic_number == EEPROM_MAGIC_NUMBER_STATS && s1.crc16 == crc1_calc) { stats = s1; LOG_INFO("eeprom_load", "Stats slot 1 OK");}
    else { memset(&stats, 0, sizeof(stats)); stats.magic_number = EEPROM_MAGIC_NUMBER_STATS; LOG_WARN("eeprom_load", "Stats invalid, resetting");}
    lastSavedStats = stats;
}
void saveStats(bool force) {
    if (!force && millis() - lastEepromStatsWrite < EEPROM_STATS_WRITE_INTERVAL) return;
    if (force || memcmp(&stats, &lastSavedStats, sizeof(stats)) != 0) {
        stats.crc16 = crc16((uint8_t*)&stats, sizeof(stats) - sizeof(stats.crc16));
        static uint8_t slot = 0;
        slot = (slot + 1) % 2;
        EEPROM.put(slot == 0 ? EEPROM_STATS_SLOT_0_ADDR : EEPROM_STATS_SLOT_1_ADDR, stats);
        lastSavedStats = stats;
        lastEepromStatsWrite = millis();
    }
}

// =================================================================
// 10. COMMAND-LINE INTERFACE (CLI)
// =================================================================
void processCLICommand() {
    char* command = strtok(cliBuffer, " ");
    if (command == NULL) return;

    if (strcmp(command, "help") == 0) {
        Serial.println(F("--- SmartVase CLI ---"));
        Serial.println(F("help             - Mostra questo menu"));
        Serial.println(F("status           - Mostra stato operativo"));
        Serial.println(F("stats            - Mostra statistiche cumulative"));
        Serial.println(F("config           - Mostra configurazione attuale"));
        Serial.println(F("sensors          - Mostra valori sensori"));
        Serial.println(F("reboot           - Riavvia il microcontrollore"));
        Serial.println(F("motor <dir> <ms> - Test motori (dir: f,b,l,r)"));
        Serial.println(F("pump <ms>        - Test pompa"));
    } else if (strcmp(command, "status") == 0) {
        Serial.print(F("Uptime (s): ")); Serial.println(millis() / 1000);
        Serial.print(F("Free RAM (B): ")); Serial.println(freeRam());
        Serial.print(F("Movement State: ")); Serial.println(currentMovementState);
        Serial.print(F("Target Mode: ")); Serial.println(targetMode);
        Serial.print(F("Degraded Mode: ")); Serial.print(systemStatus.degradedModeActive ? "SI" : "NO");
        if(systemStatus.degradedModeActive) { Serial.print(F(" -> Reason: ")); Serial.println(systemStatus.degradedReason); }
        Serial.print(F("Hub Missing: ")); Serial.println(systemStatus.hubIsMissing ? "SI" : "NO");
    } else if (strcmp(command, "stats") == 0) {
        Serial.println(F("--- Cumulative Stats ---"));
        Serial.print(F("WDT Resets: ")); Serial.println(stats.watchdog_resets);
        Serial.print(F("Irrigations: ")); Serial.println(stats.total_irrigations);
        Serial.print(F("Obstacles Avoided: ")); Serial.println(stats.obstacles_avoided);
        Serial.print(F("Stuck Events: ")); Serial.println(stats.stuck_events);
        Serial.print(F("PB Decode Fails: ")); Serial.println(stats.pb_decode_failures);
    } else if (strcmp(command, "config") == 0) {
        Serial.println(F("--- Device Config ---"));
        Serial.print(F("Motor Calib Left: ")); Serial.println(config.motorCalibLeft);
        Serial.print(F("Motor Calib Right: ")); Serial.println(config.motorCalibRight);
        Serial.print(F("Avoid Reverse (ms): ")); Serial.println(config.avoid_reverse_ms);
        Serial.print(F("Avoid Turn (ms): ")); Serial.println(config.avoid_turn_ms);
    } else if (strcmp(command, "sensors") == 0) {
        Serial.println(F("--- Sensor Values ---"));
        Serial.print(F("Front Dist (cm): ")); Serial.println(cached_front_dist_cm);
        Serial.print(F("Left Dist (cm): ")); Serial.println(cached_left_dist_cm);
        Serial.print(F("Right Dist (cm): ")); Serial.println(cached_right_dist_cm);
        Serial.print(F("Water Tank (cm): ")); Serial.println(cached_water_level_cm);
        Serial.print(F("Light (lux): ")); Serial.println(cached_lux);
        Serial.print(F("Battery (V): ")); Serial.println(cached_battery_voltage);
    } else if (strcmp(command, "reboot") == 0) {
        Serial.println(F("Rebooting..."));
        softReset();
    } else if (strcmp(command, "motor") == 0) {
        char* dir = strtok(NULL, " ");
        char* ms_str = strtok(NULL, " ");
        if (dir && ms_str) {
            int ms = atoi(ms_str);
            Serial.print(F("Motor test: dir=")); Serial.print(dir);
            Serial.print(F(" ms=")); Serial.println(ms);
            
            if (strcmp(dir, "f") == 0) moveForward();
            else if (strcmp(dir, "b") == 0) moveBackward();
            else if (strcmp(dir, "l") == 0) turnLeft();
            else if (strcmp(dir, "r") == 0) turnRight();
            
            delay(ms);
            stopMotors();
            Serial.println(F("Motor test complete."));
        } else {
            Serial.println(F("Uso: motor <f|b|l|r> <durata_ms>"));
        }
    } else if (strcmp(command, "pump") == 0) {
        char* ms_str = strtok(NULL, " ");
        if (ms_str) {
            int ms = atoi(ms_str);
            Serial.print(F("Pump test: ")); Serial.print(ms); Serial.println(" ms");
            digitalWrite(pumpPin, HIGH);
            delay(ms);
            digitalWrite(pumpPin, LOW);
            Serial.println(F("Pump test complete."));
        } else {
            Serial.println(F("Uso: pump <durata_ms>"));
        }
    } else {
        Serial.println(F("Comando non riconosciuto. Digita 'help'."));
    }
}

void handleCLI() {
    while (Serial.available() > 0) {
        char receivedChar = Serial.read();
        if (receivedChar == '\n' || receivedChar == '\r') {
            if (cliBufferPos > 0) {
                cliBuffer[cliBufferPos] = '\0';
                Serial.print(F("\n> COMANDO: ")); Serial.println(cliBuffer);
                processCLICommand();
                cliBufferPos = 0;
            }
            Serial.print(F("\nSmartVase> "));
        } else if (cliBufferPos < sizeof(cliBuffer) - 1) {
            if (isPrintable(receivedChar)) {
               cliBuffer[cliBufferPos++] = receivedChar;
               Serial.print(receivedChar); // Echo
            }
        }
    }
}

// =================================================================
// 11. SETUP & MAIN LOOP
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println(F("\nInizializzazione Platform Controller v3.7..."));
    
    Serial1.begin(115200);
    myRTC.begin();
    setSyncProvider(getRTCtime);

    mcusr_mirror = MCUSR;
    MCUSR = 0;
    wdt_disable();

    loadConfig();
    loadStats();

    if (mcusr_mirror & (1 << WDRF)) {
        stats.watchdog_resets++;
        saveStats(true);
        LOG_CRITICAL("reboot_wdt", "Watchdog reset");
    } else {
        LOG_INFO("boot", "Power-on reset");
    }
    wdt_enable(WDTO_4S);

    pinMode(pumpPin, OUTPUT); pinMode(uvaPin, OUTPUT); pinMode(forkPowerPin, OUTPUT);
    pinMode(batteryPin, INPUT);
    digitalWrite(forkPowerPin, LOW);
    pinMode(enA, OUTPUT); pinMode(in1, OUTPUT); pinMode(in2, OUTPUT);
    pinMode(enB, OUTPUT); pinMode(in3, OUTPUT); pinMode(in4, OUTPUT);

    if (!bme.begin(0x76)) {
        systemStatus.bmeSensorError = true;
        LOG_ERROR("sensor_init_fail", "BME680");
    }
    
    LOG_INFO("system_boot", "Platform Controller ready");
    Serial.println(F("Setup completo. Digita 'help' per i comandi."));
    Serial.print(F("SmartVase> "));
}

void loop() {
    wdt_reset();
    unsigned long currentMillis = millis();

    handleCLI();
    handleSerial();
    sampleSensors();
    handlePumping();
    handleSoilReading();
    handleMovementSM();

    for (int i = 0; i < numTasks; i++) {
        unsigned long interval = tasks[i].interval;
        if (systemStatus.degradedModeActive) interval *= 2;
        if (currentMillis - tasks[i].lastRun >= interval) {
            if (currentMillis - tasks[i].lastRun > interval * 2) {
                if (strcmp(tasks[i].name, "fast_telemetry") == 0) stats.task_miss_fast_telemetry++;
                else if (strcmp(tasks[i].name, "deep_telemetry") == 0) stats.task_miss_deep_telemetry++;
                else if (strcmp(tasks[i].name, "send_log") == 0) stats.task_miss_sendlog++;
                else if (strcmp(tasks[i].name, "heartbeat") == 0) stats.task_miss_heartbeat++;
            }
            tasks[i].func();
            tasks[i].lastRun = currentMillis;
        }
    }
    
    saveStats(false);

    if (freeRam() < LOW_MEMORY_THRESHOLD_BYTES) {
        enterDegradedMode("Low SRAM");
    }
    
    if (currentMillis - lastHubMessageTime > HUB_DEADMAN_TIMEOUT_MS) {
        if (!systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = true;
            enterDegradedMode("Hub Missing");
        }
    }
}
