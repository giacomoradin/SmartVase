/*!
 * @file Ultrasonic.cpp
 * @ingroup MegaSensors
 * @brief Implementation of the HC-SR04 driver: trigger/echo cycle and time-of-flight to distance conversion.
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#include "Ultrasonic.h"

/** @brief Round-trip time of sound in air: ~58 us per cm traveled, at 20 °C. */
#define US_ROUNDTRIP_US_PER_CM 58UL

Ultrasonic::Ultrasonic(uint8_t triggerPin, uint8_t echoPin, uint16_t maxDistanceCm)
    : _triggerPin(triggerPin),
      _echoPin(echoPin),
      // +25% margin over the maximum expected time of flight.
      _timeoutUs((unsigned long)maxDistanceCm * US_ROUNDTRIP_US_PER_CM * 5UL / 4UL)
{
}

void Ultrasonic::begin() {
    pinMode(_triggerPin, OUTPUT);
    pinMode(_echoPin, INPUT);
    digitalWrite(_triggerPin, LOW);
}

float Ultrasonic::readCm() {
    digitalWrite(_triggerPin, LOW);
    delayMicroseconds(4);
    digitalWrite(_triggerPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(_triggerPin, LOW);

    unsigned long pulseUs = pulseIn(_echoPin, HIGH, _timeoutUs);
    if (pulseUs == 0) return NAN;

    return (float)pulseUs / (float)US_ROUNDTRIP_US_PER_CM;
}
