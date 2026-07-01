/*!
    @file   SensorPolicy.h

    @ingroup MegaSensors

    @brief  Pure decisions derived from sensor readings.

    @details `static inline` functions with no hardware dependency, shared
             between the firmware (`Sensors.cpp`, `Movement.cpp`) and the host
             unit tests in `tests/host/` (see `test_sensor_policy`). They
             encapsulate the safety/behavior rules derived from analog or
             ultrasonic readings, separated from the actual HW access.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef SENSOR_POLICY_H
#define SENSOR_POLICY_H

#include <stdint.h>
#include <math.h>   // isnan, NAN

/*!
    @addtogroup MegaSensors
    @{
*/

/*!
    @brief    Determines whether the water tank should be considered empty.

    @details  Fail-safe: if the US4 sensor reading (on the tank) is not valid
              (`NaN`, e.g. ultrasonic timeout) the tank is considered empty and
              the pump is blocked, because without a reliable measurement
              irrigation cannot be authorized. US4 looks at the water from
              above, so a larger distance corresponds to a lower water level:
              beyond `thresholdCm` the tank is empty.

    @param[in] waterLevelCm US4->water-surface distance, in cm (`NAN` if invalid).
    @param[in] thresholdCm  Threshold beyond which the tank is considered empty, in cm.

    @return   `true` if the tank is empty (or the reading is not reliable), `false` otherwise.
*/
static inline bool tankConsideredEmpty(float waterLevelCm, uint16_t thresholdCm) {
    return isnan(waterLevelCm) || waterLevelCm > (float)thresholdCm;
}

/*!
    @brief    Determines whether the robot must turn during light/shadow seeking.

    @details  In `LIGHT` mode the robot seeks light: if `lux` is below the
              threshold (too dark) it must turn. In `SHADOW` mode it seeks
              shade: if `lux` is above the threshold (too much light) it must
              turn. `lux < 0` indicates that the photoresistor ADC reading is
              not valid yet, in which case it does not turn (fail-safe: it goes
              straight until a reliable reading is available).

    @param[in] seekingLight  `true` if the current target mode is `LIGHT`.
    @param[in] seekingShadow `true` if the current target mode is `SHADOW`.
    @param[in] lux           Current brightness ADC value (negative = invalid).
    @param[in] threshold     Configured brightness ADC threshold (`light_threshold`).

    @return   `true` if the robot must turn towards the sought condition.
*/
static inline bool seekWantsTurn(bool seekingLight, bool seekingShadow,
                                 int lux, int threshold) {
    if (lux < 0) return false;
    if (seekingLight)  return lux < threshold;
    if (seekingShadow) return lux > threshold;
    return false;
}

/*!
    @brief    Determines whether the grow lights (UVA) should be turned on.

    @details  The lights should be turned on only when the robot is stationary
              in `IDLE` mode (not actively seeking light/shadow) and the ambient
              brightness is insufficient, to supplement the plant's lighting
              while the robot is not moving. `lux < 0` indicates that the
              photoresistor ADC reading is not valid yet: in that case the
              current state is kept instead of forcing a change (fail-safe,
              consistent with ::seekWantsTurn).

    @param[in] isIdleMode   `true` if the current target mode is `IDLE`.
    @param[in] lux          Current brightness ADC value (negative = invalid).
    @param[in] threshold    Configured brightness ADC threshold (`light_threshold`).
    @param[in] currentState Current state of the lights (used as fallback if `lux` is invalid).

    @return   `true` if the lights should be turned on.
*/
static inline bool growLightWanted(bool isIdleMode, int lux, int threshold,
                                   bool currentState) {
    if (!isIdleMode) return false;
    if (lux < 0) return currentState;
    return lux < threshold;
}

/*!
    @brief    Determines whether the current hour falls within the configured daylight window.

    @details  Used to simulate the solar day/night cycle: the grow lights must
              not turn on outside this window (typically at night) even if the
              ambient light is insufficient, so as not to keep the plant lit 24
              hours a day. Fail-safe: if the RTC time is not reliable
              (`timeValid == false`, chip absent or oscillator stopped) the
              window is considered closed, because without a certain time day
              cannot be distinguished from night.

    @param[in] timeValid `true` if the RTC has a reliable time.
    @param[in] hourOfDay Current hour (0..23), typically from `hour(epoch)` (TimeLib).
    @param[in] startHour Daylight window start hour, inclusive (e.g. 6 for 06:00).
    @param[in] endHour   Daylight window end hour, exclusive (e.g. 20 for 20:00).

    @return   `true` if we are inside the daylight window AND the time is reliable.
*/
static inline bool withinDaylightWindow(bool timeValid, uint8_t hourOfDay,
                                        uint8_t startHour, uint8_t endHour) {
    if (!timeValid) return false;
    return hourOfDay >= startHour && hourOfDay < endHour;
}

/*! @} */ // MegaSensors

#endif // SENSOR_POLICY_H
