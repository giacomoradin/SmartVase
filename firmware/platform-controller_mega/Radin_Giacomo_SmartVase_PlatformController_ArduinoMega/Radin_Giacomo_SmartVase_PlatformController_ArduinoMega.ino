/*
 * =================================================================
 * SmartVase - Platform Controller (Arduino Mega)
 * Versione: 2.4.19
 * =================================================================
 * Autore Originale: Giacomo Radin
 *
 * NOTE DI VERSIONE v2.4:
 * - Migliorata la type safety utilizzando 'enum class' per gli stati.
 * - Utilizzo di 'constexpr' per costanti note a tempo di compilazione.
 * - Aggiunti commenti esplicativi nella FSM di movimento.
 * - Nessuna modifica funzionale, solo affinamenti stilistici e di robustezza.
 */

// =================================================================
// 1. LIBRARIES
// =================================================================
#include <Adafruit_BME680.h>
#include <ArduinoJson.h>
#include <HCSR04.h>
#include <EEPROM.h>
#include <DS3232RTC.h>
#include <TimeLib.h>
#include <avr/wdt.h>
#include <limits.h>
#include <math.h> // Per isnan()

// =================================================================
// 2. CONFIGURATION & DEFINITIONS
// =================================================================

// --- System Identification ---
#define DEVICE_ID "MEGA_01"

// --- EEPROM Configuration ---
#define EEPROM_MAGIC_NUMBER_STATS  0x18BEEF18
#define EEPROM_MAGIC_NUMBER_CONFIG 0xCF6BEEF6
#define EEPROM_CONFIG_SLOT_0_ADDR 0
#define EEPROM_CONFIG_SLOT_1_ADDR 50
#define EEPROM_STATS_SLOT_0_ADDR 100
#define EEPROM_STATS_SLOT_1_ADDR 200
#define EEPROM_STATS_WRITE_INTERVAL 300000 // 5 minuti
#define EEPROM_CONFIG_WRITE_INTERVAL 60000 // 1 minuto (rate limiting)

// --- Pinout ---
const int pumpPin = 2, uvaPin = 3, forkPin = A1, photoresistorPin = A0, forkPowerPin = 51;
const int enA = 7, in1 = 43, in2 = 45; // Left Motor
const int enB = 6, in3 = 47, in4 = 49; // Right Motor
#define TRIG_PIN_WATER 12
#define ECHO_PIN_WATER 8
#define TRIG_PIN_FRONT 38
#define ECHO_PIN_FRONT 39
#define TRIG_PIN_LEFT 13
#define ECHO_PIN_LEFT 11
#define TRIG_PIN_RIGHT 9
#define ECHO_PIN_RIGHT 10

// --- System & Task Configuration ---
#define LOW_MEMORY_THRESHOLD_BYTES 800
#define SENSOR_FAILURE_THRESHOLD 5
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
#define HUB_DEADMAN_TIMEOUT_MS 120000
#define MOVEMENT_PROGRESS_THRESHOLD_CM 2.0
#define MAX_SERIAL_READ_PER_LOOP 64
#define SERIAL_MSG_TIMEOUT_MS 250
#define JSON_TX_BUFFER_SIZE 512
#define SERIAL_BUFFER_SIZE 256

// --- Logging Macros ---
#define LOG_INFO(event, detail) logEvent("info", event, detail)
#define LOG_WARN(event, detail) logEvent("warn", event, detail)
#define LOG_ERROR(event, detail) logEvent("error", event, detail)
#define LOG_CRITICAL(event, detail) logEvent("critical", event, detail)

// =================================================================
// 3. STRUCTURES, ENUMS & OBJECTS
// =================================================================

// --- Hardware Objects ---
Adafruit_BME680 bme;
DS3232RTC myRTC;
UltraSonicDistanceSensor waterSensor(TRIG_PIN_WATER, ECHO_PIN_WATER);
UltraSonicDistanceSensor frontSensor(TRIG_PIN_FRONT, ECHO_PIN_FRONT);
UltraSonicDistanceSensor leftSensor(TRIG_PIN_LEFT, ECHO_PIN_LEFT);
UltraSonicDistanceSensor rightSensor(TRIG_PIN_RIGHT, ECHO_PIN_RIGHT);

// --- System State ---
struct SystemStatus {
    bool bmeSensorError; bool lowMemoryDetected; bool logQueueOverflow; bool degradedModeActive; bool hubIsMissing;
};
SystemStatus systemStatus = {false, false, false, false, false};

// ENUM CLASS for better type safety
enum class MovementState { IDLE, MOVING, AVOID_START, AVOID_REVERSING, AVOID_TURNING, STUCK };
enum class Mode { LIGHT, SHADOW, IDLE };

// --- Persistent Data Structures (con wear-leveling) ---
struct DeviceConfig {
    uint32_t magic_number; uint16_t crc16; int motorCalibLeft; int motorCalibRight; uint16_t avoid_reverse_ms; uint16_t avoid_turn_ms;
};
DeviceConfig config;

struct CumulativeStats {
    uint32_t magic_number; uint16_t crc16; uint32_t watchdog_resets; uint32_t total_irrigations; uint32_t obstacles_avoided;
    uint32_t bme_read_errors; uint32_t low_memory_events; uint32_t log_overflows; uint32_t total_irrigation_duration_s;
    uint32_t total_motor_active_time_s; uint32_t cmd_crc_failures; uint32_t serial_overflow_drops; uint32_t stuck_events;
    uint32_t escape_attempts; uint32_t no_progress_events;
    uint16_t task_miss_fast_telemetry; uint16_t task_miss_deep_telemetry; uint16_t task_miss_flushlog; uint16_t task_miss_heartbeat;
};
CumulativeStats stats, lastSavedStats;

// --- Task Scheduler ---
void sendFastTelemetry(); void sendDeepTelemetry(); void flushLogQueue(); void sendHeartbeat(); void saveStats(bool force = false);
struct Task { const char* name; void (*func)(); unsigned long interval; unsigned long lastRun; };
Task tasks[] = {
    { "fast_telemetry", sendFastTelemetry, 5000, 0 }, { "deep_telemetry", sendDeepTelemetry, 60000, 0 },
    { "flush_log", flushLogQueue, 500, 0 }, { "heartbeat", sendHeartbeat, 15000, 0 }
};
constexpr int numTasks = sizeof(tasks) / sizeof(Task);

// --- Log Queue ---
struct LogEntry { char level[10]; char event[32]; char detail[64]; };
LogEntry logQueue[LOG_QUEUE_SIZE];

// =================================================================
// 4. GLOBAL VARIABLES
// =================================================================
char jsonTxBuffer[JSON_TX_BUFFER_SIZE];
char serialBuffer[SERIAL_BUFFER_SIZE];
int bufferPos = 0;
uint8_t currentConfigEepromSlot = 0, currentStatsEepromSlot = 0;
unsigned long lastHubMessageTime = 0;
unsigned long lastEepromConfigWrite = 0, lastEepromStatsWrite = 0;
uint8_t mcusr_mirror __attribute__ ((section (".noinit")));

// --- State Variables ---
MovementState currentMovementState = MovementState::IDLE;
Mode targetMode = Mode::IDLE;
unsigned long motorActiveStartTime = 0;
unsigned long stateStartTime = 0;
unsigned long pumpStartTime = 0, pumpDuration = 0;
bool isPumping = false;
bool soilReadingRequested = false;
unsigned long soilReadingStartTime = 0;
unsigned long lastWaterCommandTime = 0;
unsigned long commandStartTime = 0;
bool testMode = false;

// --- Sensor Caching & Filtering ---
float cached_water_level_cm = NAN, cached_front_dist_cm = NAN, cached_left_dist_cm = NAN, cached_right_dist_cm = NAN;
int cached_lux = -1;
unsigned int invalid_streak_water = 0, invalid_streak_front = 0, invalid_streak_left = 0, invalid_streak_right = 0;
unsigned long lastFrontSampleTime = 0, lastSideSampleTime = 0, lastWaterSampleTime = 0, lastLightSampleTime = 0;

// --- Movement FSM ---
uint8_t avoidance_attempts = 0;
unsigned long stuck_cooldown_start_time = 0;
uint32_t current_stuck_backoff = STUCK_COOLDOWN_MS;
float pre_maneuver_dist = 0;
int lightThreshold = 600;

// --- Logging ---
int logQueueHead = 0, logQueueTail = 0, logQueueCount = 0;

// =================================================================
// 5. UTILITY & DIAGNOSTIC FUNCTIONS
// =================================================================

int freeRam() { extern int __heap_start, *__brkval; int v; return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); }
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) { mcusr_mirror = MCUSR; MCUSR = 0; wdt_disable(); }
time_t getRTCtime() { return myRTC.get(); }
uint16_t crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) { crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1; }
    }
    return crc;
}
void enterDegradedMode(const char* reason) {
    if (!systemStatus.degradedModeActive) {
        systemStatus.degradedModeActive = true;
        stopMotors();
        currentMovementState = MovementState::IDLE;
        LOG_CRITICAL("degraded_mode_entered", reason);
    }
}
void softReset() {
    LOG_WARN("soft_reset", "Initiating software reset");
    delay(100);
    wdt_enable(WDTO_15MS);
    while (true);
}

// Custom implementation of strrstr for AVR compatibility
const char* strrstr_custom(const char* haystack, const char* needle) {
    if (*needle == '\0') return haystack + strlen(haystack);
    const char* result = NULL;
    for (;;) {
        const char* p = strstr(haystack, needle);
        if (p == NULL) break;
        result = p;
        haystack = p + 1;
    }
    return result;
}

// =================================================================
// 6. LOGGING & COMMUNICATION FUNCTIONS
// =================================================================

void logEvent(const char* level, const char* event, const char* detail) {
    noInterrupts();
    if (logQueueCount >= LOG_QUEUE_SIZE) {
        if (!systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = true; stats.log_overflows++; }
        interrupts(); return;
    }
    LogEntry& entry = logQueue[logQueueTail];
    strncpy(entry.level, level, sizeof(entry.level) - 1); entry.level[sizeof(entry.level) - 1] = '\0';
    strncpy(entry.event, event, sizeof(entry.event) - 1); entry.event[sizeof(entry.event) - 1] = '\0';
    if (detail) { strncpy(entry.detail, detail, sizeof(entry.detail) - 1); entry.detail[sizeof(entry.detail) - 1] = '\0'; }
    else { entry.detail[0] = '\0'; }
    logQueueTail = (logQueueTail + 1) % LOG_QUEUE_SIZE;
    logQueueCount++;
    interrupts();
}

void sendJsonWithCrc(JsonDocument& doc, long cmdIdForNak = 0);

void sendNak(long cmdId, const char* reason, const char* detail) {
    StaticJsonDocument<192> doc;
    doc["type"] = "nak";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    doc["cmd_id"] = cmdId;
    doc["reason_code"] = reason;
    if (detail) doc["detail"] = detail;
    sendJsonWithCrc(doc);
}

void sendJsonWithCrc(JsonDocument& doc, long cmdIdForNak) {
    if (doc.containsKey("crc16")) doc.remove("crc16");
    size_t required_size = measureJson(doc);
    if (required_size + 25 > JSON_TX_BUFFER_SIZE) {
        LOG_ERROR("send_json_failed", "Message too large");
        if (cmdIdForNak != 0) sendNak(cmdIdForNak, "oversize", "Response payload too large");
        return;
    }
    size_t len_no_crc = serializeJson(doc, jsonTxBuffer, JSON_TX_BUFFER_SIZE);
    doc["crc16"] = crc16((const uint8_t*)jsonTxBuffer, len_no_crc);
    serializeJson(doc, Serial1);
    Serial1.println();
}

void sendAck(long cmdId, const char* ackFor, bool success, const char* detail, unsigned long execution_time_ms = 0) {
    StaticJsonDocument<256> doc;
    doc["type"] = "ack";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    doc["cmd_id"] = cmdId;
    doc["ack_for"] = ackFor;
    doc["status"] = success ? "ok" : "error";
    if (detail) doc["detail"] = detail;
    if (execution_time_ms > 0) doc["execution_time_ms"] = execution_time_ms;
    sendJsonWithCrc(doc, cmdId);
}

bool validateCrcField(const char* json, size_t len, uint16_t& calculated_crc) {
    const char* crc_ptr = strrstr_custom(json, ",\"crc16\":");
    if (!crc_ptr) return false;
    const char* after_crc = crc_ptr + strlen(",\"crc16\":");
    while(*after_crc && isspace(*after_crc)) after_crc++;
    while(*after_crc && isdigit(*after_crc)) after_crc++;
    while(*after_crc && isspace(*after_crc)) after_crc++;
    if (*after_crc != '}') return false;

    size_t payload_len = (size_t)(crc_ptr - json);
    calculated_crc = crc16((const uint8_t*)json, payload_len);
    return true;
}

void executeCommand(const char* cmdJson, size_t len) {
    lastHubMessageTime = millis();
    commandStartTime = millis();

    uint16_t calculated_crc;
    if (!validateCrcField(cmdJson, len, calculated_crc)) {
        LOG_ERROR("cmd_parse_error", "CRC field missing or malformed");
        sendNak(0, "missing_crc", nullptr);
        return;
    }
    StaticJsonDocument<SERIAL_BUFFER_SIZE> doc;
    DeserializationError error = deserializeJson(doc, cmdJson);
    long cmdId = doc["cmd_id"] | 0;
    if (error) { LOG_ERROR("json_parse_failed", error.c_str()); sendNak(cmdId, "parse_error", error.c_str()); return; }
    uint16_t received_crc = doc["crc16"] | 0;
    if (received_crc != calculated_crc) {
        stats.cmd_crc_failures++;
        char detail[64];
        snprintf(detail, sizeof(detail), "Rcv:%u Calc:%u", received_crc, calculated_crc);
        LOG_ERROR("cmd_crc_mismatch", detail);
        sendNak(cmdId, "crc_mismatch", detail);
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) { sendNak(cmdId, "parse_error", "Missing 'cmd' field"); return; }
    
    if (strcmp(cmd, "diagnose") == 0) {
        sendDeepTelemetry();
        sendAck(cmdId, cmd, true, "Deep telemetry sent");
    } else if (strcmp(cmd, "soft_reset") == 0) {
        sendAck(cmdId, cmd, true, "Soft reset initiated");
        softReset();
    } else if (strcmp(cmd, "stop") == 0) {
        targetMode = Mode::IDLE;
        currentMovementState = MovementState::IDLE;
        stopMotors();
        sendAck(cmdId, cmd, true, "Motors stopped");
    } else if (strcmp(cmd, "set_motion_params") == 0) {
        JsonObject params = doc["params"];
        uint16_t reverse_ms = params["reverse_ms"] | config.avoid_reverse_ms;
        uint16_t turn_ms = params["turn_ms"] | config.avoid_turn_ms;
        if (reverse_ms >= 200 && reverse_ms <= 3000 && turn_ms >= 200 && turn_ms <= 3000) {
            config.avoid_reverse_ms = reverse_ms;
            config.avoid_turn_ms = turn_ms;
            saveConfig();
            sendAck(cmdId, cmd, true, "Motion params updated");
        } else {
            sendAck(cmdId, cmd, false, "Invalid parameter range");
        }
    } else if (strcmp(cmd, "set_mode") == 0) {
        const char* mode_str = doc["mode"];
        if (strcmp(mode_str, "light") == 0) targetMode = Mode::LIGHT;
        else if (strcmp(mode_str, "shadow") == 0) targetMode = Mode::SHADOW;
        else targetMode = Mode::IDLE;
        sendAck(cmdId, cmd, true, "Mode updated");
    } else if (systemStatus.degradedModeActive || systemStatus.hubIsMissing) {
        sendAck(cmdId, cmd, false, "System in safe/degraded mode");
    } else if (strcmp(cmd, "water") == 0) {
        if (isPumping) { sendAck(cmdId, cmd, false, "Pump already active"); return; }
        if (cached_water_level_cm > WATER_TANK_EMPTY_CM) { sendAck(cmdId, cmd, false, "Water tank is empty"); LOG_ERROR("pump_safety_abort", "Tank empty"); return; }
        if (millis() - lastWaterCommandTime < COMMAND_DEBOUNCE_MS) { sendAck(cmdId, cmd, false, "Debounce active"); return; }
        
        unsigned long duration = doc["duration_ms"] | 5000;
        isPumping = true;
        pumpStartTime = millis();
        pumpDuration = min(duration, MAX_PUMP_DURATION_MS);
        if(!testMode) digitalWrite(pumpPin, HIGH);
        stats.total_irrigations++;
        lastWaterCommandTime = millis();
        sendAck(cmdId, cmd, true, "Irrigation started");
    } else if (strcmp(cmd, "read_soil") == 0) {
        if (!soilReadingRequested) {
            digitalWrite(forkPowerPin, HIGH);
            soilReadingStartTime = millis();
            soilReadingRequested = true;
            sendAck(cmdId, cmd, true, "Soil reading initiated");
        } else {
            sendAck(cmdId, cmd, false, "Reading already in progress");
        }
    } else {
        sendAck(cmdId, cmd, false, "Unknown command");
    }
}

void handleSerial() {
    while (Serial1.available()) {
        char c = Serial1.read();
        if (bufferPos == 0 && c != '{') continue;
        if (c == '\n') {
            if (bufferPos > 0) {
                serialBuffer[bufferPos] = '\0';
                executeCommand(serialBuffer, bufferPos);
            }
            bufferPos = 0;
        } else if (bufferPos < SERIAL_BUFFER_SIZE - 1) {
            serialBuffer[bufferPos++] = c;
        } else {
            stats.serial_overflow_drops++;
            LOG_ERROR("serial_overflow_drop", "Incoming message too large");
            bufferPos = 0;
            while (Serial1.available() > 0 && Serial1.read() != '\n');
        }
    }
}

void flushLogQueue() {
    if (Serial1.availableForWrite() < 256) return;
    if (logQueueCount == 0) {
        if (systemStatus.logQueueOverflow) { systemStatus.logQueueOverflow = false; LOG_INFO("log_queue_recovered", nullptr); }
        return;
    }
    noInterrupts();
    LogEntry& entry = logQueue[logQueueHead];
    logQueueHead = (logQueueHead + 1) % LOG_QUEUE_SIZE;
    logQueueCount--;
    interrupts();
    
    StaticJsonDocument<JSON_TX_BUFFER_SIZE> doc;
    doc["type"] = "log";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    doc["level"] = entry.level;
    doc["event"] = entry.event;
    if (strlen(entry.detail) > 0) doc["detail"] = entry.detail;
    sendJsonWithCrc(doc);
}

void sendHeartbeat() {
    StaticJsonDocument<JSON_TX_BUFFER_SIZE> doc;
    doc["type"] = "heartbeat";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    JsonObject status = doc.createNestedObject("status");
    status["bme_error"] = systemStatus.bmeSensorError;
    status["low_mem"] = systemStatus.lowMemoryDetected;
    status["log_overflow"] = systemStatus.logQueueOverflow;
    status["degraded_mode"] = systemStatus.degradedModeActive;
    status["hub_missing"] = systemStatus.hubIsMissing;
    status["free_ram_bytes"] = freeRam();
    sendJsonWithCrc(doc);
}

void sendFastTelemetry() {
    StaticJsonDocument<JSON_TX_BUFFER_SIZE> doc;
    doc["type"] = "telemetry_fast";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    JsonObject data = doc.createNestedObject("data");
    data["water_level_cm"] = cached_water_level_cm;
    data["front_dist_cm"] = cached_front_dist_cm;
    data["movement_state"] = static_cast<int>(currentMovementState);
    data["lux"] = cached_lux;
    data["free_ram_bytes"] = freeRam();
    doc["payload_bytes"] = measureJson(doc);
    sendJsonWithCrc(doc);
}

void sendDeepTelemetry() {
    StaticJsonDocument<1024> doc;
    doc["type"] = "telemetry_deep";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = now();
    
    JsonObject data = doc.createNestedObject("data");
    readBmeSensor(data);

    JsonObject perf = doc.createNestedObject("performance");
    perf["uptime_s"] = millis() / 1000;
    perf["task_miss_fast_telemetry"] = stats.task_miss_fast_telemetry;
    perf["task_miss_deep_telemetry"] = stats.task_miss_deep_telemetry;
    perf["task_miss_flushlog"] = stats.task_miss_flushlog;
    perf["task_miss_heartbeat"] = stats.task_miss_heartbeat;
    
    JsonObject cum = doc.createNestedObject("cumulative");
    cum["watchdog_resets"] = stats.watchdog_resets;
    cum["total_irrigations"] = stats.total_irrigations;
    cum["obstacles_avoided"] = stats.obstacles_avoided;
    cum["stuck_events"] = stats.stuck_events;
    cum["escape_attempts"] = stats.escape_attempts;
    cum["no_progress_events"] = stats.no_progress_events;
    cum["cmd_crc_failures"] = stats.cmd_crc_failures;
    cum["serial_overflow_drops"] = stats.serial_overflow_drops;

    doc["payload_bytes"] = measureJson(doc);
    sendJsonWithCrc(doc);
}

// =================================================================
// 7. SENSOR & CORE LOGIC FUNCTIONS
// =================================================================

float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak, float min_valid, float max_valid) {
    const float alpha = 0.4;
    if (raw_value < min_valid || raw_value > max_valid) {
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
}

void readBmeSensor(JsonObject& data) {
    if (!bme.performReading()) {
        stats.bme_read_errors++;
        if (!systemStatus.bmeSensorError) { systemStatus.bmeSensorError = true; LOG_WARN("sensor_unreliable", "BME680 failed"); }
        data["temperature_c"] = nullptr; data["humidity_percent"] = nullptr; data["pressure_hpa"] = nullptr; data["gas_ohms"] = nullptr;
        return;
    }
    if (systemStatus.bmeSensorError) { systemStatus.bmeSensorError = false; LOG_INFO("sensor_recovered", "BME680 responding"); }
    data["temperature_c"] = bme.temperature; 
    data["humidity_percent"] = bme.humidity;
    data["pressure_hpa"] = bme.pressure / 100.0F;
    data["gas_ohms"] = bme.gas_resistance;
}

void handleSoilReading() {
    if (soilReadingRequested && millis() - soilReadingStartTime >= 50) {
        int soilRaw = analogRead(forkPin);
        digitalWrite(forkPowerPin, LOW);
        soilReadingRequested = false;
        char buf[10]; snprintf(buf, sizeof(buf), "%d", soilRaw);
        LOG_INFO("soil_reading_done", buf);
    }
}

void handlePumping() {
    if (isPumping && millis() - pumpStartTime >= pumpDuration) {
        if(!testMode) digitalWrite(pumpPin, LOW);
        isPumping = false;
        stats.total_irrigation_duration_s += pumpDuration / 1000;
        LOG_INFO("irrigation_end", "Cycle finished");
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

void moveForward() { if (testMode) return; analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }
void moveBackward() { if (testMode) return; analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void turnRight() { if (testMode) return; analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void turnLeft() { if (testMode) return; analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }

void handleMovementSM() {
    if (systemStatus.degradedModeActive || systemStatus.hubIsMissing) { if (currentMovementState != MovementState::IDLE) { stopMotors(); currentMovementState = MovementState::IDLE; } return; }
    if (motorActiveStartTime > 0 && millis() - motorActiveStartTime > MOTOR_SAFETY_TIMEOUT_MS) { LOG_ERROR("motor_timeout", nullptr); stopMotors(); currentMovementState = MovementState::IDLE; return; }

    bool front_obs = !isnan(cached_front_dist_cm) && cached_front_dist_cm < OBSTACLE_DISTANCE_CM;
    bool left_obs = !isnan(cached_left_dist_cm) && cached_left_dist_cm < OBSTACLE_DISTANCE_CM;
    bool right_obs = !isnan(cached_right_dist_cm) && cached_right_dist_cm < OBSTACLE_DISTANCE_CM;

    switch (currentMovementState) {
        case MovementState::IDLE:
            stopMotors();
            if (targetMode != Mode::IDLE) { 
                currentMovementState = MovementState::MOVING; 
                motorActiveStartTime = millis(); 
                avoidance_attempts = 0; 
                current_stuck_backoff = STUCK_COOLDOWN_MS; 
            }
            break;
        case MovementState::MOVING:
            if (front_obs) { 
                stats.obstacles_avoided++; 
                currentMovementState = MovementState::AVOID_START; // Transition to avoidance
            } 
            else { // Continue normal movement
                if (targetMode == Mode::LIGHT && cached_lux < lightThreshold) turnRight();
                else if (targetMode == Mode::SHADOW && cached_lux > lightThreshold) turnLeft();
                else moveForward();
            }
            break;
        case MovementState::AVOID_START:
            stopMotors();
            pre_maneuver_dist = cached_front_dist_cm;
            stateStartTime = millis();
            currentMovementState = MovementState::AVOID_REVERSING;
            stats.escape_attempts++;
            break;
        case MovementState::AVOID_REVERSING:
            moveBackward();
            if (millis() - stateStartTime > config.avoid_reverse_ms) { 
                stateStartTime = millis(); 
                currentMovementState = MovementState::AVOID_TURNING; 
            }
            break;
        case MovementState::AVOID_TURNING:
            if (millis() - stateStartTime > config.avoid_turn_ms) {
                if (!isnan(pre_maneuver_dist) && !isnan(cached_front_dist_cm) && abs(cached_front_dist_cm - pre_maneuver_dist) < MOVEMENT_PROGRESS_THRESHOLD_CM) { stats.no_progress_events++; }
                if (front_obs) { // Still blocked after turning
                    avoidance_attempts++;
                    if (avoidance_attempts >= STUCK_DETECTION_THRESHOLD) {
                        stats.stuck_events++;
                        LOG_CRITICAL("stuck_detected", "Entering STUCK state");
                        currentMovementState = MovementState::STUCK;
                        stuck_cooldown_start_time = millis();
                    } else { 
                        currentMovementState = MovementState::AVOID_START; // Retry avoidance
                    }
                } else { 
                    currentMovementState = MovementState::IDLE; // Path is clear, return to IDLE before MOVING
                }
                break;
            }
            // Turn logic
            if (!right_obs && right_obs != left_obs) { turnRight(); }
            else if (!left_obs) { turnLeft(); }
            else { if (random(0, 2) == 0) turnLeft(); else turnRight(); }
            break;
        case MovementState::STUCK:
            stopMotors();
            if (millis() - stuck_cooldown_start_time > current_stuck_backoff) {
                LOG_INFO("stuck_cooldown_end", "Attempting recovery");
                currentMovementState = MovementState::IDLE;
                current_stuck_backoff += STUCK_BACKOFF_INCREMENT_MS;
            }
            break;
    }
}

// =================================================================
// 8. PERSISTENCE FUNCTIONS
// =================================================================

void loadConfig() {
    DeviceConfig c0, c1;
    EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
    EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
    uint16_t crc0 = crc16((uint8_t*)&c0, sizeof(c0) - sizeof(c0.crc16));
    uint16_t crc1 = crc16((uint8_t*)&c1, sizeof(c1) - sizeof(c1.crc16));
    if (c0.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c0.crc16 == crc0) { config = c0; currentConfigEepromSlot = 0; }
    else if (c1.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c1.crc16 == crc1) { config = c1; currentConfigEepromSlot = 1; }
    else { config = {EEPROM_MAGIC_NUMBER_CONFIG, 0, 255, 240, 1000, 1200}; saveConfig(); }
}
void saveConfig() {
    if (millis() - lastEepromConfigWrite < EEPROM_CONFIG_WRITE_INTERVAL) return;
    config.magic_number = EEPROM_MAGIC_NUMBER_CONFIG;
    config.crc16 = crc16((uint8_t*)&config, sizeof(config) - sizeof(config.crc16));
    currentConfigEepromSlot = (currentConfigEepromSlot + 1) % 2;
    int addr = (currentConfigEepromSlot == 0) ? EEPROM_CONFIG_SLOT_0_ADDR : EEPROM_CONFIG_SLOT_1_ADDR;
    EEPROM.put(addr, config);
    lastEepromConfigWrite = millis();
}
void loadStats() {
    CumulativeStats s0, s1;
    EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
    EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);
    uint16_t crc0 = crc16((uint8_t*)&s0, sizeof(s0) - sizeof(s0.crc16));
    uint16_t crc1 = crc16((uint8_t*)&s1, sizeof(s1) - sizeof(s1.crc16));
    if (s0.magic_number == EEPROM_MAGIC_NUMBER_STATS && s0.crc16 == crc0) { stats = s0; currentStatsEepromSlot = 0; }
    else if (s1.magic_number == EEPROM_MAGIC_NUMBER_STATS && s1.crc16 == crc1) { stats = s1; currentStatsEepromSlot = 1; }
    else { memset(&stats, 0, sizeof(stats)); stats.magic_number = EEPROM_MAGIC_NUMBER_STATS; saveStats(true); }
    lastSavedStats = stats;
}
void saveStats(bool force) {
    if (!force && millis() - lastEepromStatsWrite < EEPROM_STATS_WRITE_INTERVAL) return;
    stats.crc16 = crc16((uint8_t*)&stats, sizeof(stats) - sizeof(stats.crc16));
    if (force || memcmp(&stats, &lastSavedStats, sizeof(stats)) != 0) {
        currentStatsEepromSlot = (currentStatsEepromSlot + 1) % 2;
        int addr = (currentStatsEepromSlot == 0) ? EEPROM_STATS_SLOT_0_ADDR : EEPROM_STATS_SLOT_1_ADDR;
        EEPROM.put(addr, stats);
        lastSavedStats = stats;
    }
    lastEepromStatsWrite = millis();
}

// =================================================================
// 9. SETUP & MAIN LOOP
// =================================================================

void setup() {
    Serial.begin(115200); Serial1.begin(115200);
    myRTC.begin(); setSyncProvider(getRTCtime);
    randomSeed(analogRead(A5) ^ micros());

    loadConfig();
    loadStats();

    if (mcusr_mirror & (1 << WDRF)) {
        stats.watchdog_resets++;
        LOG_CRITICAL("watchdog_reset", "System recovered from WDT");
        saveStats(true);
    }
    wdt_enable(WDTO_4S);

    pinMode(pumpPin, OUTPUT); pinMode(uvaPin, OUTPUT); pinMode(forkPowerPin, OUTPUT);
    digitalWrite(forkPowerPin, LOW);
    pinMode(enA, OUTPUT); pinMode(in1, OUTPUT); pinMode(in2, OUTPUT);
    pinMode(enB, OUTPUT); pinMode(in3, OUTPUT); pinMode(in4, OUTPUT);

    if (!bme.begin(0x76)) { systemStatus.bmeSensorError = true; LOG_ERROR("sensor_init_failed", "BME680"); }
    
    lastHubMessageTime = millis();
    LOG_INFO("system_boot", "Platform Controller initialized");
}

void loop() {
    wdt_reset();
    unsigned long currentMillis = millis();

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
                else if (strcmp(tasks[i].name, "flush_log") == 0) stats.task_miss_flushlog++;
                else if (strcmp(tasks[i].name, "heartbeat") == 0) stats.task_miss_heartbeat++;
            }
            tasks[i].func();
            tasks[i].lastRun = currentMillis;
        }
    }
    
    saveStats(false);

    if (freeRam() < LOW_MEMORY_THRESHOLD_BYTES) enterDegradedMode("Low SRAM");
    
    if (currentMillis - lastHubMessageTime > HUB_DEADMAN_TIMEOUT_MS) {
        if (!systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = true;
            LOG_CRITICAL("hub_missing", "Entering safe mode");
            currentMovementState = MovementState::IDLE; stopMotors();
        }
    } else if (systemStatus.hubIsMissing) {
        systemStatus.hubIsMissing = false;
        LOG_INFO("hub_recovered", "Communication restored");
    }
}
