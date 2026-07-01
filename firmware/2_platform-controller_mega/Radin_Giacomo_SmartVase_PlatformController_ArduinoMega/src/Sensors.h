/*!
 * @file Sensors.h
 * @ingroup MegaSensors
 * @brief Unified sensor-reading module of the Platform Controller (ultrasonic, ADC, RTC, BME680).
 * @date 2026-04-29
 * @author Giacomo Radin
 */

/**
 * @defgroup MegaSensors Sensors and RTC (Mega)
 * @brief Round-robin ultrasonic sensor reading with EMA, soil/light/battery ADC, optional BME680, DS3232 RTC, pure sensor-interpretation policies.
 * @{
 */

#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "Ultrasonic.h"
#include "RtcDs3232.h"
#include "smartvase_aliases.h"
#include "SensorPolicy.h"

// --- Feature flags ---
/**
 * @def BME680_ENABLED
 * @brief Enable flag for the BME680 temperature/pressure/humidity/gas sensor.
 *
 * The BME680 is not fitted on the current prototype (PIN map 2026-05-19, confirmed 2026-06-11).
 * Keep at 0 until it is wired. If set to 1, re-enables the I2C probe, readings in TelemetryDeep and logging.
 */
#define BME680_ENABLED 0

/**
 * @def BATTERY_MONITORING_ENABLED
 * @brief Enable flag for battery voltage monitoring.
 *
 * The battery is not yet wired on the new prototype. If set to 1, re-enables the voltage divider readings.
 */
#define BATTERY_MONITORING_ENABLED 0

#if BME680_ENABLED
#include <Adafruit_BME680.h>
#endif

/**
 * @class Sensors
 * @brief Module for unified management of SmartVase's sensors (ultrasonic, photoresistor, hygrometer, RTC, BME680).
 *
 * Performs the readings of the 6 HC-SR04 ultrasonic sensors in non-blocking round-robin mode (one reading every
 * `US_SAMPLE_INTERVAL_MS`, see Sensors.cpp), applies EMA (Exponential Moving Average) filters to stabilize
 * noisy readings, and provides filtered/processed data both locally (for Movement, CLI/serial commands) and
 * for building the Protobuf telemetry packets (`TelemetryFast`/`TelemetryDeep`).
 *
 * @note No blocking calls in the polling: `sampleSensors()` must be called at high frequency from the non-blocking
 *       main loop (no `delay()`), consistent with the Mega firmware conventions.
 */
class Sensors {
public:
    /**
     * @brief Constructor of the Sensors class.
     */
    Sensors();

    /**
     * @brief Initializes the sensor pins and the communication buses (I2C/Wire for RTC and BME).
     */
    void init();

    /**
     * @brief Performs the non-blocking round-robin polling of the 6 ultrasonic sensors and the analog inputs.
     *
     * To be called at high frequency inside the main loop.
     */
    void sampleSensors();

    // --- Filtered readings (cm) ---
    /** @brief Returns the distance from sensor US1 (Front Top) in cm. */
    float getTopDist()        const { return cached_top_dist_cm; }
    /** @brief Returns the distance from sensor US2 (Front Right) in cm. */
    float getFrontRightDist() const { return cached_front_right_dist_cm; }
    /** @brief Returns the distance from sensor US3 (Front Left) in cm. */
    float getFrontLeftDist()  const { return cached_front_left_dist_cm; }
    /** @brief Returns the water level reading from the tank sensor US4, in cm. */
    float getWaterLevel()     const { return cached_water_level_cm; }
    /** @brief Returns the distance from sensor US5 (Side Left) in cm. */
    float getLeftDist()       const { return cached_left_dist_cm; }
    /** @brief Returns the distance from sensor US6 (Side Right) in cm. */
    float getRightDist()      const { return cached_right_dist_cm; }

    /**
     * @brief Checks whether the water tank is empty.
     *
     * Performs a fail-safe check: whether the US4 reading is invalid or exceeds the empty-tank threshold.
     *
     * @param thresholdCm Water level limit threshold in cm (sensor-to-water distance).
     * @return true if the tank is considered empty or the reading is invalid.
     */
    bool tankLooksEmpty(uint16_t thresholdCm) const {
        return tankConsideredEmpty(cached_water_level_cm, thresholdCm);
    }

    // --- ADC ---
    /** @brief Returns the value read from the LDR (light level, 0-1023). */
    int getLux()           const { return cached_lux; }
    /** @brief Returns the soil moisture value from the hygrometer (0-1023). */
    int getSoilMoisture()  const { return cached_soil_moisture; }
    /** @brief Returns the estimated battery voltage in Volts. */
    float getBatteryVoltage() const { return cached_battery_voltage; }

    // --- RTC ---
    /**
     * @brief Reads and returns the current UNIX epoch.
     * @details Uses the real RTC chip if present and reachable; otherwise, if a
     *          software fallback clock has been set (see setEpoch()), returns
     *          that (based on `millis()`, keeps advancing realistically).
     * @return UNIX epoch in seconds, or 0 if no time source is available.
     */
    uint32_t getEpoch();
    /**
     * @brief Sets the current UNIX epoch.
     * @details First attempts to write to the real RTC chip; if the chip is not detected or
     *          the I2C write fails, falls back to a software clock (`millis()`-based) that
     *          starts from `epoch_s` and keeps advancing as long as the Mega stays powered — useful
     *          for bring-up when the CR2032 backup battery is low/absent and the chip
     *          does not respond, without having to wait for a hardware replacement. The software clock is
     *          lost on every reset/power-off and must be set again.
     * @param epoch_s UNIX timestamp to set (in seconds).
     * @return true (the operation is always taken on, real or software; use
     *         isUsingFakeClock() to find out which of the two was used).
     */
    bool setEpoch(uint32_t epoch_s);
    /** @brief Returns the operating status of the real RTC chip (does not account for the software clock). */
    bool getRTCStatus() const { return rtc_status; }
    /** @brief Checks whether the real RTC chip's oscillator has stopped (e.g. dead backup battery). */
    bool rtcOscStopped() { return rtc_status ? rtc.oscillatorStopped() : true; }
    /**
     * @brief Checks whether a reliable time is available, from the real chip or from the software fallback clock.
     * @details To be used instead of `getRTCStatus() && !rtcOscStopped()` when only interested in
     *          knowing whether `getEpoch()` will return a plausible value, regardless of the
     *          source (see setEpoch() for the software fallback).
     * @return true if the real chip has a valid time or the software clock is active.
     */
    bool timeIsValid();
    /** @brief true if the current time comes from the software fallback clock rather than the real RTC chip. */
    bool isUsingFakeClock() const { return fake_clock_active; }

    // --- BME680 ---
    /** @brief Returns the operating/detection status of the BME680 sensor. */
    bool getBMEStatus() const { return bme_status; }

    /**
     * @brief Builds the TelemetryFast Protobuf message.
     *
     * @param movState Current state of the motor state machine.
     * @param deviceId Mega device ID.
     * @return TelemetryFast Structure populated with the fast telemetry.
     */
    TelemetryFast buildFastTelemetry(CppMovementState movState, const char* deviceId);

    /**
     * @brief Builds the TelemetryDeep Protobuf message.
     *
     * @param stats Cumulative statistics to send to the cloud.
     * @param deviceId Mega device ID.
     * @return TelemetryDeep Structure populated with the deep telemetry.
     */
    TelemetryDeep buildDeepTelemetry(CumulativeStats& stats, const char* deviceId);

private:
    /**
     * @brief Applies an Exponential Moving Average (EMA) filter to a raw reading, discarding values
     *        outside the valid range and falling back to NAN after consecutive invalid readings.
     *
     * @param[in] raw_value     Latest raw sensor reading (can be NAN if the sensor timed out).
     * @param[in] last_value    Previous filtered value (filter state), used as the base of the moving average.
     * @param[in,out] invalid_streak Counter of consecutive invalid readings; reset on the first valid reading,
     *                          incremented otherwise. Past an internal threshold, the filter "forgets" its state
     *                          and returns NAN (see Sensors.cpp for the exact threshold value).
     * @param[in] min_valid     Lower bound of the plausible range for the reading (cm).
     * @param[in] max_valid     Upper bound of the plausible range for the reading (cm), typically the maximum
     *                          range configured for the HC-SR04 probe.
     * @return Filtered value (EMA), or NAN if the sensor is considered unreliable.
     */
    float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak,
                         float min_valid, float max_valid);
    /**
     * @brief Triggers and reads the next ultrasonic sensor in the round-robin sequence.
     * @note Internally advances the cycle index (`us_cycle_idx`) over the 6 probes; updates the
     *       corresponding filtered (EMA) cache and its consecutive invalid-reading counter.
     */
    void  sampleNextUltrasonic();
    /**
     * @brief Samples the analog ADC channels (lux, hygrometer, battery voltage if enabled).
     * @note Applies a light EMA filter on lux and soil moisture to stabilize noisy readings; the
     *       battery reading is gated by the #BATTERY_MONITORING_ENABLED flag.
     */
    void  sampleAdcChannels();

#if BME680_ENABLED
    Adafruit_BME680 bme; /**< Driver for the BME680 environmental sensor (active only if #BME680_ENABLED == 1). */
#endif
    RtcDs3232 rtc; /**< Local driver for the DS3232 RTC (I2C 0x68). */

    Ultrasonic us1_top;          /**< Front Top ultrasonic sensor (US1) */
    Ultrasonic us2_front_right;  /**< Front Right ultrasonic sensor (US2) */
    Ultrasonic us3_front_left;   /**< Front Left ultrasonic sensor (US3) */
    Ultrasonic us4_water;        /**< Tank Water Level ultrasonic sensor (US4) */
    Ultrasonic us5_left;         /**< Side Left ultrasonic sensor (US5) */
    Ultrasonic us6_right;        /**< Side Right ultrasonic sensor (US6) */

    uint8_t us_cycle_idx;        /**< Index for managing sequential polling (0..5) */
    unsigned long last_us_sample_ms;/**< Timestamp of the last ultrasonic sensor sample */

    float cached_top_dist_cm;          /**< Latest filtered (EMA) US1 distance, in cm; NAN if invalid. */
    float cached_front_right_dist_cm;  /**< Latest filtered (EMA) US2 distance, in cm; NAN if invalid. */
    float cached_front_left_dist_cm;   /**< Latest filtered (EMA) US3 distance, in cm; NAN if invalid. */
    float cached_water_level_cm;       /**< Latest filtered (EMA) US4 distance (tank), in cm; NAN if invalid. */
    float cached_left_dist_cm;         /**< Latest filtered (EMA) US5 distance, in cm; NAN if invalid. */
    float cached_right_dist_cm;        /**< Latest filtered (EMA) US6 distance, in cm; NAN if invalid. */

    unsigned int invalid_streak_top;    /**< Consecutive out-of-range/invalid readings for US1. */
    unsigned int invalid_streak_fr;     /**< Consecutive out-of-range/invalid readings for US2. */
    unsigned int invalid_streak_fl;     /**< Consecutive out-of-range/invalid readings for US3. */
    unsigned int invalid_streak_water;  /**< Consecutive out-of-range/invalid readings for US4. */
    unsigned int invalid_streak_left;   /**< Consecutive out-of-range/invalid readings for US5. */
    unsigned int invalid_streak_right;  /**< Consecutive out-of-range/invalid readings for US6. */

    int   cached_lux;             /**< Latest filtered LDR ADC value (0-1023); -1 if not sampled yet. */
    int   cached_soil_moisture;   /**< Latest filtered soil-moisture fork ADC value (0-1023); -1 if not sampled yet. */
    float cached_battery_voltage; /**< Latest estimated battery voltage (V); NAN if monitoring is disabled or not read. */

    bool bme_status; /**< true if the BME680 was detected and successfully initialized in init(). */
    bool rtc_status;  /**< true if the DS3232 RTC responded on I2C in init(). */

    bool          fake_clock_active;     /**< true if the software fallback clock is in use (see setEpoch()). */
    uint32_t      fake_clock_base_epoch; /**< Epoch set by the user when the software clock was activated. */
    unsigned long fake_clock_set_millis; /**< millis() at activation time, used to compute elapsed time. */
};

#endif // SENSORS_H

/** @} */ // end of MegaSensors
