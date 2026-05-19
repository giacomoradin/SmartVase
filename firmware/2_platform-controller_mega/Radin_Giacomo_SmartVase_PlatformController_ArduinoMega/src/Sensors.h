#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <Adafruit_BME680.h>
#include <HCSR04.h>
#include <DS3232RTC.h>
#include "smartvase_aliases.h"

// --- Feature flags ---
// La batteria non e' ancora cablata sul nuovo prototipo (PIN map 2026-05-19).
// Quando si aggiunge il partitore resistivo, settare a 1 e definire i pin
// e i valori R1/R2 in Sensors.cpp.
#define BATTERY_MONITORING_ENABLED 0

class Sensors {
public:
    Sensors();
    void init();
    // Polling round-robin dei 6 HC-SR04 + ADC veloci (lux, soil).
    // Da chiamare ad alta frequenza nel main loop.
    void sampleSensors();

    // --- Letture filtrate (cm) ---
    float getTopDist()        const { return cached_top_dist_cm; }        // US1
    float getFrontRightDist() const { return cached_front_right_dist_cm; }// US2
    float getFrontLeftDist()  const { return cached_front_left_dist_cm; } // US3
    float getWaterLevel()     const { return cached_water_level_cm; }     // US4
    float getLeftDist()       const { return cached_left_dist_cm; }       // US5
    float getRightDist()      const { return cached_right_dist_cm; }      // US6

    // --- ADC ---
    int getLux()           const { return cached_lux; }
    int getSoilMoisture()  const { return cached_soil_moisture; }
    float getBatteryVoltage() const { return cached_battery_voltage; }

    // --- RTC ---
    uint32_t getEpoch();
    bool getRTCStatus() const { return rtc_status; }

    // --- BME680 ---
    bool getBMEStatus() const { return bme_status; }

    // Riempie un TelemetryFast con i valori cached + stato movimento + device_id
    TelemetryFast buildFastTelemetry(CppMovementState movState, const char* deviceId);

    // Riempie un TelemetryDeep leggendo BME680 + counters cumulativi
    TelemetryDeep buildDeepTelemetry(CumulativeStats& stats, const char* deviceId);

private:
    float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak,
                         float min_valid, float max_valid);
    void  sampleNextUltrasonic();
    void  sampleAdcChannels();

    Adafruit_BME680 bme;
    DS3232RTC       rtc;

    // 6 HC-SR04 — pin dal PIN map autoritativo (docs/PINS - Sheet1.csv)
    HCSR04 us1_top;          // trig 33, echo 35
    HCSR04 us2_front_right;  // trig 26, echo 27
    HCSR04 us3_front_left;   // trig 36, echo 37
    HCSR04 us4_water;        // trig 50, echo 51
    HCSR04 us5_left;         // trig  4, echo  5
    HCSR04 us6_right;        // trig 28, echo 29

    // Round-robin index 0..5 sulle 6 sonde
    uint8_t us_cycle_idx;
    unsigned long last_us_sample_ms;

    // Cache letture (cm) — NAN se non ancora valido
    float cached_top_dist_cm;
    float cached_front_right_dist_cm;
    float cached_front_left_dist_cm;
    float cached_water_level_cm;
    float cached_left_dist_cm;
    float cached_right_dist_cm;

    // Contatori di letture invalide consecutive (per fail-safe)
    unsigned int invalid_streak_top;
    unsigned int invalid_streak_fr;
    unsigned int invalid_streak_fl;
    unsigned int invalid_streak_water;
    unsigned int invalid_streak_left;
    unsigned int invalid_streak_right;

    int   cached_lux;
    int   cached_soil_moisture;
    float cached_battery_voltage;

    bool bme_status;
    bool rtc_status;
};

#endif // SENSORS_H
