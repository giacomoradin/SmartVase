#include "Sensors.h"
#include <Wire.h>

// =================================================================
// PIN MAP — fonte autoritativa: docs/PINS - Sheet1.csv (2026-05-19)
// =================================================================

// Sensori HC-SR04 (trigger, echo)
#define US1_TOP_TRIG          33
#define US1_TOP_ECHO          35
#define US2_FRONT_RIGHT_TRIG  26
#define US2_FRONT_RIGHT_ECHO  27
#define US3_FRONT_LEFT_TRIG   36
#define US3_FRONT_LEFT_ECHO   37
#define US4_WATER_TRIG        50
#define US4_WATER_ECHO        51
#define US5_LEFT_TRIG          4
#define US5_LEFT_ECHO          5
#define US6_RIGHT_TRIG        28
#define US6_RIGHT_ECHO        29

// Portata massima per sonda: limita il timeout del pulseIn e quindi il tempo
// di blocco del main loop per ogni campionamento (~14 ms a 200 cm).
// La tanica e' profonda al massimo qualche decina di cm: timeout piu' corto.
#define US_NAV_MAX_CM        200
#define US_WATER_MAX_CM      120

// ADC analogici
#define SOIL_MOISTURE_PIN     A0   // Forcella umidita' suolo
#define PHOTORESISTOR_PIN     A1   // Spostato da A0 per liberare la forcella

#if BATTERY_MONITORING_ENABLED
// TODO: confermare a banco quando il partitore resistivo viene cablato.
#define BATTERY_PIN           A2
#define ADC_REFERENCE_VOLTAGE 5.0f
#define VOLTAGE_DIVIDER_R1    30000.0f
#define VOLTAGE_DIVIDER_R2     7500.0f
#endif

#if BME680_ENABLED
// Indirizzi I2C
#define BME680_I2C_ADDRESS    0x76
#endif

// Throttling lettura ultrasuoni: una sonda ogni N ms (round-robin sui 6).
// 6 sonde × 30 ms => refresh completo ~180 ms.
#define US_SAMPLE_INTERVAL_MS 30

Sensors::Sensors() :
    us1_top         (US1_TOP_TRIG,         US1_TOP_ECHO,         US_NAV_MAX_CM),
    us2_front_right (US2_FRONT_RIGHT_TRIG, US2_FRONT_RIGHT_ECHO, US_NAV_MAX_CM),
    us3_front_left  (US3_FRONT_LEFT_TRIG,  US3_FRONT_LEFT_ECHO,  US_NAV_MAX_CM),
    us4_water       (US4_WATER_TRIG,       US4_WATER_ECHO,       US_WATER_MAX_CM),
    us5_left        (US5_LEFT_TRIG,        US5_LEFT_ECHO,        US_NAV_MAX_CM),
    us6_right       (US6_RIGHT_TRIG,       US6_RIGHT_ECHO,       US_NAV_MAX_CM),
    us_cycle_idx(0),
    last_us_sample_ms(0),
    cached_top_dist_cm(NAN),
    cached_front_right_dist_cm(NAN),
    cached_front_left_dist_cm(NAN),
    cached_water_level_cm(NAN),
    cached_left_dist_cm(NAN),
    cached_right_dist_cm(NAN),
    invalid_streak_top(0),
    invalid_streak_fr(0),
    invalid_streak_fl(0),
    invalid_streak_water(0),
    invalid_streak_left(0),
    invalid_streak_right(0),
    cached_lux(-1),
    cached_soil_moisture(-1),
    cached_battery_voltage(NAN),
    bme_status(false),
    rtc_status(false)
{
}

void Sensors::init() {
    pinMode(SOIL_MOISTURE_PIN, INPUT);
    pinMode(PHOTORESISTOR_PIN, INPUT);
#if BATTERY_MONITORING_ENABLED
    pinMode(BATTERY_PIN, INPUT);
#endif

    us1_top.begin();
    us2_front_right.begin();
    us3_front_left.begin();
    us4_water.begin();
    us5_left.begin();
    us6_right.begin();

    Wire.begin();

#if BME680_ENABLED
    // BME680 (T / RH / pressione / VOC)
    if (bme.begin(BME680_I2C_ADDRESS)) {
        bme.setTemperatureOversampling(BME680_OS_8X);
        bme.setHumidityOversampling(BME680_OS_2X);
        bme.setPressureOversampling(BME680_OS_4X);
        bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
        bme.setGasHeater(320, 150); // 320 C per 150 ms
        bme_status = true;
    }
#endif

    // RTC DS3232 (I2C 0x68)
    rtc_status = rtc.begin();
}

float Sensors::applyEmaFilter(float raw_value, float last_value,
                              unsigned int& invalid_streak,
                              float min_valid, float max_valid) {
    const float alpha = 0.4f;
    if (isnan(raw_value) || raw_value < min_valid || raw_value > max_valid) {
        invalid_streak++;
        // Dopo 10 letture invalide consecutive il valore diventa NAN.
        return (invalid_streak >= 10) ? NAN : last_value;
    }
    invalid_streak = 0;
    if (isnan(last_value)) return raw_value;
    return (alpha * raw_value) + ((1.0f - alpha) * last_value);
}

void Sensors::sampleNextUltrasonic() {
    // Range valido HC-SR04: sotto i 2 cm le letture non sono affidabili.
    const float MIN_DIST  = 2.0f;
    const float MAX_NAV   = (float)US_NAV_MAX_CM;
    const float MAX_WATER = (float)US_WATER_MAX_CM;

    float raw = NAN;
    switch (us_cycle_idx) {
        case 0:
            raw = us1_top.readCm();
            cached_top_dist_cm = applyEmaFilter(raw, cached_top_dist_cm,
                                                invalid_streak_top, MIN_DIST, MAX_NAV);
            break;
        case 1:
            raw = us2_front_right.readCm();
            cached_front_right_dist_cm = applyEmaFilter(raw, cached_front_right_dist_cm,
                                                        invalid_streak_fr, MIN_DIST, MAX_NAV);
            break;
        case 2:
            raw = us3_front_left.readCm();
            cached_front_left_dist_cm = applyEmaFilter(raw, cached_front_left_dist_cm,
                                                       invalid_streak_fl, MIN_DIST, MAX_NAV);
            break;
        case 3:
            raw = us4_water.readCm();
            cached_water_level_cm = applyEmaFilter(raw, cached_water_level_cm,
                                                   invalid_streak_water, MIN_DIST, MAX_WATER);
            break;
        case 4:
            raw = us5_left.readCm();
            cached_left_dist_cm = applyEmaFilter(raw, cached_left_dist_cm,
                                                 invalid_streak_left, MIN_DIST, MAX_NAV);
            break;
        case 5:
            raw = us6_right.readCm();
            cached_right_dist_cm = applyEmaFilter(raw, cached_right_dist_cm,
                                                  invalid_streak_right, MIN_DIST, MAX_NAV);
            break;
    }
    us_cycle_idx = (us_cycle_idx + 1) % 6;
}

void Sensors::sampleAdcChannels() {
    cached_lux           = analogRead(PHOTORESISTOR_PIN);
    cached_soil_moisture = analogRead(SOIL_MOISTURE_PIN);

#if BATTERY_MONITORING_ENABLED
    int rawAdc = analogRead(BATTERY_PIN);
    float v_adc = (rawAdc / 1023.0f) * ADC_REFERENCE_VOLTAGE;
    cached_battery_voltage = v_adc * ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2);
#endif
}

void Sensors::sampleSensors() {
    unsigned long now = millis();
    if (now - last_us_sample_ms >= US_SAMPLE_INTERVAL_MS) {
        sampleNextUltrasonic();
        last_us_sample_ms = now;
    }
    // ADC veloci: ogni chiamata.
    sampleAdcChannels();
}

uint32_t Sensors::getEpoch() {
    if (!rtc_status) return 0;
    time_t t = rtc.get();
    return (t == 0) ? 0 : (uint32_t)t;
}

bool Sensors::setEpoch(uint32_t epoch_s) {
    if (!rtc_status) return false;
    return rtc.set((time_t)epoch_s);
}

TelemetryFast Sensors::buildFastTelemetry(CppMovementState movState, const char* deviceId) {
    TelemetryFast tf = TelemetryFast_init_zero;
    tf.top_dist_cm         = isnan(cached_top_dist_cm)         ? 0.0f : cached_top_dist_cm;
    tf.front_right_dist_cm = isnan(cached_front_right_dist_cm) ? 0.0f : cached_front_right_dist_cm;
    tf.front_left_dist_cm  = isnan(cached_front_left_dist_cm)  ? 0.0f : cached_front_left_dist_cm;
    tf.left_dist_cm        = isnan(cached_left_dist_cm)        ? 0.0f : cached_left_dist_cm;
    tf.right_dist_cm       = isnan(cached_right_dist_cm)       ? 0.0f : cached_right_dist_cm;
    tf.water_level_cm      = isnan(cached_water_level_cm)      ? 0.0f : cached_water_level_cm;
    tf.soil_moisture       = (cached_soil_moisture < 0) ? 0 : cached_soil_moisture;
    tf.lux                 = (cached_lux < 0) ? 0 : cached_lux;
    tf.movement_state      = cppMovementStateToProto(movState);
    tf.epoch_s             = getEpoch();
    if (deviceId) {
        strncpy(tf.device_id, deviceId, sizeof(tf.device_id) - 1);
        tf.device_id[sizeof(tf.device_id) - 1] = '\0';
    }
    return tf;
}

extern int freeRam(); // definito in main.cpp

TelemetryDeep Sensors::buildDeepTelemetry(CumulativeStats& stats, const char* deviceId) {
    TelemetryDeep td = TelemetryDeep_init_zero;

#if BME680_ENABLED
    if (bme_status) {
        if (bme.performReading()) {
            td.temperature_c       = bme.temperature;
            td.humidity_percent    = bme.humidity;
            td.pressure_hpa        = bme.pressure / 100.0f;
            td.gas_resistance_ohms = (uint32_t)bme.gas_resistance;
        } else {
            stats.bme_read_errors++;
        }
    }
#endif

    td.uptime_s       = millis() / 1000UL;
    td.free_ram_bytes = (uint32_t)freeRam();
    td.epoch_s        = getEpoch();

    td.watchdog_resets             = stats.watchdog_resets;
    td.total_irrigations           = stats.total_irrigations;
    td.obstacles_avoided           = stats.obstacles_avoided;
    td.stuck_events                = stats.stuck_events;
    td.bme_read_errors             = stats.bme_read_errors;
    td.log_overflows               = stats.log_overflows;
    td.total_irrigation_duration_s = stats.total_irrigation_duration_s;
    td.total_motor_active_time_s   = stats.total_motor_active_time_s;
    td.pb_decode_failures          = stats.pb_decode_failures;

#if BATTERY_MONITORING_ENABLED
    if (!isnan(cached_battery_voltage)) {
        td.battery_voltage = cached_battery_voltage;
    }
#endif

    if (deviceId) {
        strncpy(td.device_id, deviceId, sizeof(td.device_id) - 1);
        td.device_id[sizeof(td.device_id) - 1] = '\0';
    }
    return td;
}
