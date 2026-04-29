#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <Adafruit_BME680.h>
#include <HCSR04.h>
#include "smartvase_aliases.h"

class Sensors {
public:
    Sensors();
    void init();
    void sampleSensors();
    float getBatteryVoltage();
    float getWaterLevel();
    float getFrontDistance();
    float getLeftDistance();
    float getRightDistance();
    int getLux();
    bool getBMEStatus();
    TelemetryDeep getDeepTelemetry(CumulativeStats& stats);


private:
    float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak, float min_valid, float max_valid);

    Adafruit_BME680 bme;
    HCSR04 waterSensor;
    HCSR04 frontSensor;
    HCSR04 leftSensor;
    HCSR04 rightSensor;

    float cached_battery_voltage;
    float cached_water_level_cm;
    float cached_front_dist_cm;
    float cached_left_dist_cm;
    float cached_right_dist_cm;
    int cached_lux;
    unsigned int invalid_streak_water;
    unsigned int invalid_streak_front;
    unsigned int invalid_streak_left;
    unsigned int invalid_streak_right;
    bool bme_status;
};

#endif // SENSORS_H