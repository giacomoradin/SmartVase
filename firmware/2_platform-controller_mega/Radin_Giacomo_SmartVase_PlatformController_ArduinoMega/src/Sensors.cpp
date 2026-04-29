#include "Sensors.h"

// --- Pinout ---
const int photoresistorPin = A0;
const int batteryPin = A2;
#define TRIG_PIN_WATER 12
#define ECHO_PIN_WATER 8
#define TRIG_PIN_FRONT 38
#define ECHO_PIN_FRONT 39
#define TRIG_PIN_LEFT 13
#define ECHO_PIN_LEFT 11
#define TRIG_PIN_RIGHT 9
#define ECHO_PIN_RIGHT 10

// --- Costanti per Monitoraggio Batteria (da calibrare) ---
#define ADC_REFERENCE_VOLTAGE 5.0
#define VOLTAGE_DIVIDER_R1 30000.0 // Valore R1 del partitore
#define VOLTAGE_DIVIDER_R2 7500.0  // Valore R2 del partitore

Sensors::Sensors() : 
    waterSensor(TRIG_PIN_WATER, ECHO_PIN_WATER),
    frontSensor(TRIG_PIN_FRONT, ECHO_PIN_FRONT),
    leftSensor(TRIG_PIN_LEFT, ECHO_PIN_LEFT),
    rightSensor(TRIG_PIN_RIGHT, ECHO_PIN_RIGHT)
{
    cached_battery_voltage = NAN;
    cached_water_level_cm = NAN;
    cached_front_dist_cm = NAN;
    cached_left_dist_cm = NAN;
    cached_right_dist_cm = NAN;
    cached_lux = -1;
    invalid_streak_water = 0;
    invalid_streak_front = 0;
    invalid_streak_left = 0;
    invalid_streak_right = 0;
    bme_status = false;
}

void Sensors::init() {
    pinMode(batteryPin, INPUT);
    if (bme.begin(0x76)) {
        bme_status = true;
    }
}

void Sensors::sampleSensors() {
    unsigned long currentMillis = millis();
    // ... (rest of the implementation from the .ino file)
}

float Sensors::applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak, float min_valid, float max_valid) {
    const float alpha = 0.4;
    if (isnan(raw_value) || raw_value < min_valid || raw_value > max_valid) {
        invalid_streak++;
        return (invalid_streak >= 10) ? NAN : last_value;
    }
    invalid_streak = 0;
    if (isnan(last_value)) return raw_value;
    return (alpha * raw_value) + ((1.0 - alpha) * last_value);
}

float Sensors::getBatteryVoltage() {
    int rawAdc = analogRead(batteryPin);
    float voltage = (rawAdc / 1023.0) * ADC_REFERENCE_VOLTAGE;
    cached_battery_voltage = voltage * ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2);
    return cached_battery_voltage;
}

float Sensors::getWaterLevel() {
    float raw = waterSensor.getDistance();
    cached_water_level_cm = applyEmaFilter(raw, cached_water_level_cm, invalid_streak_water, 2.0, 400.0);
    return cached_water_level_cm;
}

float Sensors::getFrontDistance() {
    float raw = frontSensor.getDistance();
    cached_front_dist_cm = applyEmaFilter(raw, cached_front_dist_cm, invalid_streak_front, 2.0, 400.0);
    return cached_front_dist_cm;
}

float Sensors::getLeftDistance() {
    float raw = leftSensor.getDistance();
    cached_left_dist_cm = applyEmaFilter(raw, cached_left_dist_cm, invalid_streak_left, 2.0, 400.0);
    return cached_left_dist_cm;
}

float Sensors::getRightDistance() {
    float raw = rightSensor.getDistance();
    cached_right_dist_cm = applyEmaFilter(raw, cached_right_dist_cm, invalid_streak_right, 2.0, 400.0);
    return cached_right_dist_cm;
}

int Sensors::getLux() {
    cached_lux = analogRead(photoresistorPin);
    return cached_lux;
}

bool Sensors::getBMEStatus() {
    return bme_status;
}

TelemetryDeep Sensors::getDeepTelemetry(CumulativeStats& stats) {
    TelemetryDeep td = TelemetryDeep_init_zero;

    if (bme.performReading()) {
        // if (systemStatus.bmeSensorError) { systemStatus.bmeSensorError = false; LOG_INFO("sensor_ok", "BME680"); }
        td.temperature_c = bme.temperature;
        td.humidity_percent = bme.humidity;
        td.pressure_hpa = bme.pressure / 100.0F;
        td.gas_resistance_ohms = bme.gas_resistance;
    } else {
        stats.bme_read_errors++;
        // if (!systemStatus.bmeSensorError) { systemStatus.bmeSensorError = true; LOG_WARN("sensor_fail", "BME680"); }
    }
    
    td.uptime_s = millis() / 1000;
    // td.free_ram_bytes = freeRam();
    td.watchdog_resets = stats.watchdog_resets;
    td.total_irrigations = stats.total_irrigations;
    td.obstacles_avoided = stats.obstacles_avoided;
    td.stuck_events = stats.stuck_events;
    td.bme_read_errors = stats.bme_read_errors;
    td.log_overflows = stats.log_overflows;
    td.total_irrigation_duration_s = stats.total_irrigation_duration_s;
    td.total_motor_active_time_s = stats.total_motor_active_time_s;
    td.pb_decode_failures = stats.pb_decode_failures;
    // strncpy(td.device_id, DEVICE_ID, sizeof(td.device_id) - 1);
    
    if (!isnan(cached_battery_voltage)) {
        td.battery_voltage = cached_battery_voltage;
    }

    return td;
}
