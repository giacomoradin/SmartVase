/*!
 * @file Sensors.cpp
 * @ingroup MegaSensors
 * @brief Implementation of the Sensors module: pin map, sampling constants, EMA filters, telemetry building.
 * @date 2026-04-29
 * @author Giacomo Radin
 */

#include "Sensors.h"
#include <Wire.h>

// =================================================================
// PIN MAP — authoritative source: docs/PINS - Sheet1.csv (2026-05-19)
// =================================================================

/*! @name HC-SR04 pins (TRIG/ECHO) — authoritative source: docs/PINS - Sheet1.csv
 *  @{ */
#define US1_TOP_TRIG          33  ///< US1 front top, TRIG.
#define US1_TOP_ECHO          35  ///< US1 front top, ECHO.
#define US2_FRONT_RIGHT_TRIG  27  ///< US2 front right, TRIG.
#define US2_FRONT_RIGHT_ECHO  26  ///< US2 front right, ECHO.
#define US3_FRONT_LEFT_TRIG   37  ///< US3 front left, TRIG.
#define US3_FRONT_LEFT_ECHO   36  ///< US3 front left, ECHO.
#define US4_WATER_TRIG        50  ///< US4 tank level, TRIG.
#define US4_WATER_ECHO        51  ///< US4 tank level, ECHO.
#define US5_LEFT_TRIG          4  ///< US5 side left, TRIG.
#define US5_LEFT_ECHO          5  ///< US5 side left, ECHO.
#define US6_RIGHT_TRIG        28  ///< US6 side right, TRIG.
#define US6_RIGHT_ECHO        29  ///< US6 side right, ECHO.
/*! @} */

/*! @name Maximum range per probe (cm)
 *  @details Limits the `pulseIn` timeout and therefore the main loop's blocking
 *           time for each sample (~14 ms at 200 cm). The tank is at most a few
 *           tens of cm deep, so it uses a shorter timeout.
 *  @{ */
#define US_NAV_MAX_CM        200  ///< Range of the navigation probes (US1/2/3/5/6).
#define US_WATER_MAX_CM      120  ///< Range of the tank probe (US4).
/*! @} */

// Analog ADC channels
#define SOIL_MOISTURE_PIN     A0   ///< Soil moisture fork (ADC).
#define PHOTORESISTOR_PIN     A1   ///< LDR photoresistor (ADC), moved from A0 to free up the fork.

#if BATTERY_MONITORING_ENABLED
// TODO: confirm on the bench once the resistive voltage divider is wired.
#define BATTERY_PIN           A2        ///< Battery voltage divider (ADC).
#define ADC_REFERENCE_VOLTAGE 5.0f      ///< ADC reference voltage, in V.
#define VOLTAGE_DIVIDER_R1    30000.0f  ///< High-side resistor of the battery divider, in Ω.
#define VOLTAGE_DIVIDER_R2     7500.0f  ///< Low-side resistor of the battery divider, in Ω.
#endif

#if BME680_ENABLED
#define BME680_I2C_ADDRESS    0x76  ///< I2C address of the BME680 environmental sensor.
#endif

/*! @brief Ultrasonic reading throttling: one probe every N ms (round-robin over 6 -> full refresh ~180 ms). */
#define US_SAMPLE_INTERVAL_MS 30

/*! @brief Default hour (0-23) used in init() when no time source is available
 *         (RTC absent or time invalid): inside GrowLight's daylight window (06:00-20:00),
 *         so the system starts in a plausible "daytime" state instead of being stuck at night. */
#define DEFAULT_BOOT_HOUR 8

/*! @brief Runtime RTC re-probe period (ms): while the chip is undetected, one
           address probe per minute recovers it after a flaky contact comes
           good, without waiting for a reboot (see sampleSensors()). */
#define RTC_REPROBE_INTERVAL_MS 60000UL

/*! @brief Timezone offset (s) applied to the UTC epoch received from the Hub
           heartbeat (see syncEpochFromHub()). The wire carries UTC (NTP);
           the Mega's hour-of-day features (grow-light daylight window, care
           day anchor) want local time. 7200 = CEST (Italy, summer); set 3600
           for CET in winter. Note: `rtc set` from the CLI applies NO offset —
           pass a locally-adjusted epoch there if the exact local hour matters. */
#define HUB_EPOCH_TZ_OFFSET_S   7200L

Sensors::Sensors() :
    us1_top         (US1_TOP_TRIG,         US1_TOP_ECHO,         US_NAV_MAX_CM),
    us2_front_right (US2_FRONT_RIGHT_TRIG, US2_FRONT_RIGHT_ECHO, US_NAV_MAX_CM),
    us3_front_left  (US3_FRONT_LEFT_TRIG,  US3_FRONT_LEFT_ECHO,  US_NAV_MAX_CM),
    us4_water       (US4_WATER_TRIG,       US4_WATER_ECHO,       US_WATER_MAX_CM),
    us5_left        (US5_LEFT_TRIG,        US5_LEFT_ECHO,        US_NAV_MAX_CM),
    us6_right       (US6_RIGHT_TRIG,       US6_RIGHT_ECHO,       US_NAV_MAX_CM),
    us_cycle_idx(0),
    last_us_sample_ms(0),
    last_rtc_reprobe_ms(0),
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
    rtc_status(false),
    fake_clock_active(false),
    fake_clock_base_epoch(0),
    fake_clock_set_millis(0),
    last_hub_sync_ms(0)
{
    for (uint8_t i = 0; i < 6; ++i) {
        raw_hist0[i] = NAN;
        raw_hist1[i] = NAN;
    }
}

float Sensors::medianPrefilter(uint8_t sensorIdx, float raw) {
    // NaN-aware median over {new, previous, two-samples-ago}: rejects a single
    // spurious echo without the EMA's lag. History is a 2-slot shift register
    // per probe. See medianOf3() in SensorPolicy.h.
    if (sensorIdx >= 6) return raw;
    float med = medianOf3(raw, raw_hist0[sensorIdx], raw_hist1[sensorIdx]);
    raw_hist1[sensorIdx] = raw_hist0[sensorIdx];
    raw_hist0[sensorIdx] = raw;
    return med;
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
#if defined(ARDUINO_ARCH_AVR)
    Wire.setWireTimeout(25000, true); // 25ms timeout, reset bus on timeout
#endif

#if BME680_ENABLED
    // BME680 (T / RH / pressure / VOC)
    if (bme.begin(BME680_I2C_ADDRESS)) {
        bme.setTemperatureOversampling(BME680_OS_8X);
        bme.setHumidityOversampling(BME680_OS_2X);
        bme.setPressureOversampling(BME680_OS_4X);
        bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
        bme.setGasHeater(320, 150); // 320 C for 150 ms
        bme_status = true;
    }
#endif

    // RTC DS3232 (I2C 0x68). The single-shot probe proved fragile on the
    // bench (chip missed at boot but answering a later i2cscan): retry a few
    // times with a short settle delay before giving up. A periodic re-probe
    // in sampleSensors() then keeps recovering it at runtime.
    for (uint8_t attempt = 0; attempt < 3 && !rtc_status; ++attempt) {
        if (attempt) delay(50);
        rtc_status = rtc.begin();
    }

    // No real time available (chip absent from the bus, or present but with a
    // stopped oscillator due to a low/absent backup battery): start from a
    // plausible default time (8:00, inside GrowLight's 06:00-20:00 daylight
    // window) instead of staying stuck waiting for a manual 'rtc set' on
    // every boot. The day is arbitrary: only the hour matters.
    // SOFTWARE clock only, deliberately: writing the boot default to the chip
    // would corrupt its year register (epochs before 2000 underflow the
    // year-offset encoding in RtcDs3232::set) and a bogus-but-ticking chip
    // time would then take precedence over the fallback. The real chip is
    // adopted by the first user `rtc set` (which also clears the OSF flag).
    if (!rtc_status || rtc.oscillatorStopped()) {
        fake_clock_base_epoch = DEFAULT_BOOT_HOUR * 3600UL;
        fake_clock_set_millis = millis();
        fake_clock_active     = true;
    }
}

float Sensors::applyEmaFilter(float raw_value, float last_value,
                              unsigned int& invalid_streak,
                              float min_valid, float max_valid) {
    const float alpha = 0.4f;
    if (isnan(raw_value) || raw_value < min_valid || raw_value > max_valid) {
        invalid_streak++;
        // After 10 consecutive invalid readings, the value becomes NAN.
        return (invalid_streak >= 10) ? NAN : last_value;
    }
    invalid_streak = 0;
    if (isnan(last_value)) return raw_value;
    return (alpha * raw_value) + ((1.0f - alpha) * last_value);
}

void Sensors::sampleNextUltrasonic() {
    // Valid HC-SR04 range: below 2 cm readings are not reliable.
    const float MIN_DIST  = 2.0f;
    const float MAX_NAV   = (float)US_NAV_MAX_CM;
    const float MAX_WATER = (float)US_WATER_MAX_CM;

    // Pipeline per probe: raw read -> median-of-3 anti-bounce -> EMA + range check.
    float raw = NAN;
    switch (us_cycle_idx) {
        case 0:
            raw = medianPrefilter(0, us1_top.readCm());
            cached_top_dist_cm = applyEmaFilter(raw, cached_top_dist_cm,
                                                invalid_streak_top, MIN_DIST, MAX_NAV);
            break;
        case 1:
            raw = medianPrefilter(1, us2_front_right.readCm());
            cached_front_right_dist_cm = applyEmaFilter(raw, cached_front_right_dist_cm,
                                                        invalid_streak_fr, MIN_DIST, MAX_NAV);
            break;
        case 2:
            raw = medianPrefilter(2, us3_front_left.readCm());
            cached_front_left_dist_cm = applyEmaFilter(raw, cached_front_left_dist_cm,
                                                       invalid_streak_fl, MIN_DIST, MAX_NAV);
            break;
        case 3:
            raw = medianPrefilter(3, us4_water.readCm());
            cached_water_level_cm = applyEmaFilter(raw, cached_water_level_cm,
                                                   invalid_streak_water, MIN_DIST, MAX_WATER);
            break;
        case 4:
            raw = medianPrefilter(4, us5_left.readCm());
            cached_left_dist_cm = applyEmaFilter(raw, cached_left_dist_cm,
                                                 invalid_streak_left, MIN_DIST, MAX_NAV);
            break;
        case 5:
            raw = medianPrefilter(5, us6_right.readCm());
            cached_right_dist_cm = applyEmaFilter(raw, cached_right_dist_cm,
                                                  invalid_streak_right, MIN_DIST, MAX_NAV);
            break;
    }
    us_cycle_idx = (us_cycle_idx + 1) % 6;
}

void Sensors::sampleAdcChannels() {
    // Light EMA (alpha=0.3) on lux and soil: the LDR and fork ADCs are
    // noisy; the filter stabilizes the values used by seeking, telemetry and
    // the readSoil command without perceptible delay. Seeded on the first reading.
    const int rawLux  = analogRead(PHOTORESISTOR_PIN);
    const int rawSoil = analogRead(SOIL_MOISTURE_PIN);
    cached_lux           = (cached_lux < 0)
                           ? rawLux
                           : (int)(0.3f * rawLux  + 0.7f * cached_lux);
    cached_soil_moisture = (cached_soil_moisture < 0)
                           ? rawSoil
                           : (int)(0.3f * rawSoil + 0.7f * cached_soil_moisture);

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

    // RTC contact recovery: the boot probe is a snapshot, and a flaky wire
    // that comes good later would otherwise leave rtc_status=false until the
    // next reboot. One address probe per minute is negligible bus traffic;
    // getEpoch() keeps preferring the software clock (if active) until a
    // user `rtc set` hands the time over to the recovered chip.
    if (!rtc_status && now - last_rtc_reprobe_ms >= RTC_REPROBE_INTERVAL_MS) {
        last_rtc_reprobe_ms = now;
        rtc_status = rtc.begin();
    }
    // Fast ADCs: on every call.
    sampleAdcChannels();
}

uint32_t Sensors::getEpoch() {
    // The chip takes precedence only while the software fallback is NOT in
    // use: a chip that (re)appears at runtime with an unset time (OSF set,
    // registers at the 2000-01-01 reset defaults) must not hijack a running
    // software clock with a bogus epoch. `rtc set` remains the clean
    // hand-over: on a successful chip write it disables the fallback.
    if (rtc_status && !fake_clock_active) {
        time_t t = rtc.get();
        if (t != 0) return (uint32_t)t;
    }
    if (fake_clock_active) {
        unsigned long elapsed_s = (millis() - fake_clock_set_millis) / 1000UL;
        return fake_clock_base_epoch + (uint32_t)elapsed_s;
    }
    return 0;
}

bool Sensors::setEpoch(uint32_t epoch_s) {
    if (rtc_status && rtc.set((time_t)epoch_s)) {
        fake_clock_active = false; // real chip OK: the software fallback is no longer needed
        return true;
    }
    // Chip absent or I2C write failed (e.g. dead backup battery and the chip
    // not responding even on VCC): software fallback, see Sensors.h.
    fake_clock_base_epoch = epoch_s;
    fake_clock_set_millis = millis();
    fake_clock_active     = true;
    return true;
}

bool Sensors::timeIsValid() {
    if (rtc_status && !rtc.oscillatorStopped()) return true;
    return fake_clock_active;
}

void Sensors::syncEpochFromHub(uint32_t epoch_s) {
    const uint32_t local_epoch = epoch_s + (uint32_t)HUB_EPOCH_TZ_OFFSET_S;

    // Re-base the software clock on the fresh NTP time and keep it ACTIVE:
    // while the Hub keeps syncing (one heartbeat every ~30 s) the software
    // clock is the authoritative source (see getEpoch()), and between syncs
    // it free-runs on millis() with negligible drift.
    fake_clock_base_epoch = local_epoch;
    fake_clock_set_millis = millis();
    fake_clock_active     = true;
    last_hub_sync_ms      = millis();

    // Opportunistic hardware write: if a (healthy or replaced) RTC chip is on
    // the bus, keep it aligned too — it clears the OSF flag and lets a future
    // module hold the time across power cycles. Failure is irrelevant here:
    // the software clock above is already up to date.
    if (rtc_status) {
        (void)rtc.set((time_t)local_epoch);
    }
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

extern int freeRam(); // defined in main.cpp

TelemetryDeep Sensors::buildDeepTelemetry(CumulativeStats& stats, const char* deviceId) {
    TelemetryDeep td = TelemetryDeep_init_zero;

    // BME680 absent on the prototype: mark the ambient fields as "not measured"
    // (NaN) so the Hub can omit them from the JSON instead of publishing 0 as if
    // they were real readings. If the sensor is present, they are overwritten below.
    td.temperature_c    = NAN;
    td.humidity_percent = NAN;
    td.pressure_hpa     = NAN;
    td.gas_resistance_ohms = 0;
    td.battery_voltage  = NAN;

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
    td.light_seeking_sessions      = stats.light_seeking_sessions;
    td.shadow_seeking_sessions     = stats.shadow_seeking_sessions;
    td.escape_attempts             = stats.escape_attempts;

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
