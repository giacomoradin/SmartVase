/*!
 * @file Ultrasonic.h
 * @ingroup MegaSensors
 * @brief Local non-blocking driver for HC-SR04 ultrasonic sensors.
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <Arduino.h>

/**
 * @class Ultrasonic
 * @brief Minimal driver for a single HC-SR04 probe (trigger/echo), with configurable timeout.
 *
 * Replaces the enjoyneering/HCSR04 library, which inserts a blocking `delay(50)` inside every
 * `getDistance()`: with 6 probes in round-robin that would be ~300 ms of main-loop stall per full cycle.
 * Here the only wait is the `pulseIn` with a timeout proportional to `maxDistanceCm`, so the cost of one
 * reading stays below ~12 ms for probes with a 200 cm range.
 *
 * @note Usage constraint: do not re-read the same probe before ~60 ms (the residual echo must die out).
 *       The 6-probe round-robin with one sample every 30 ms (see Sensors.cpp) satisfies the constraint with
 *       a wide margin (180 ms per probe).
 */
class Ultrasonic {
public:
    /**
     * @brief Builds the driver for an HC-SR04 probe, deriving the read timeout from the maximum range.
     * @param[in] triggerPin   Digital pin wired to the probe TRIG pin.
     * @param[in] echoPin      Digital pin wired to the probe ECHO pin.
     * @param[in] maxDistanceCm Maximum useful range in cm; determines the internal `pulseIn` timeout
     *                          (default 200 cm if unspecified).
     */
    Ultrasonic(uint8_t triggerPin, uint8_t echoPin, uint16_t maxDistanceCm = 200);

    /**
     * @brief Configures the GPIO pins (TRIG as OUTPUT driven LOW, ECHO as INPUT).
     * @note Call once during initialization, after the pins have been assigned.
     */
    void begin();

    /**
     * @brief Runs one trigger+measure cycle and returns the detected distance.
     * @return Distance in cm, or NAN if no echo is received within the timeout
     *         (probe out of range or disconnected).
     * @note Blocks for at most `_timeoutUs` microseconds (the `pulseIn` time); the caller
     *       (Sensors::sampleNextUltrasonic) is responsible for not invoking it more often than the
     *       probe re-read constraint.
     */
    float readCm();

private:
    uint8_t       _triggerPin; /**< Probe TRIG digital pin. */
    uint8_t       _echoPin;    /**< Probe ECHO digital pin. */
    unsigned long _timeoutUs;  /**< pulseIn timeout in microseconds, derived from maxDistanceCm. */
};

#endif // ULTRASONIC_H
