/*!
    @file   GrowLight.cpp

    @ingroup MegaGrowLight

    @brief  Implementation of the grow light relay driver (see GrowLight.h).

    @date   2026-06-30

    @author Giacomo Radin
*/

#include "GrowLight.h"
#include "SensorPolicy.h"
#include <TimeLib.h>

// Authoritative PIN map: relay IN1 = D10 (pump, see Pump.cpp), IN2 = D11
// (grow lights). The relay module is active low (GPIO LOW = relay energized),
// but the lights are wired on the NC (Normally Closed) contact: with the relay
// NOT energized (at rest) the contact is closed and the lights are ON; energizing
// the relay opens the contact and the lights turn OFF. The "light on -> GPIO
// level" polarity is therefore INVERTED compared to Pump.cpp
// (wired on the NO contact, where relay energized = pump on).
#define GROWLIGHT_RELAY_PIN       11    ///< Grow light relay pin (channel 2, D11).
#define GROWLIGHT_GPIO_LEVEL_ON   HIGH  ///< GPIO level to turn on: at rest (not energized) = NC closed = lights ON.
#define GROWLIGHT_GPIO_LEVEL_OFF  LOW   ///< GPIO level to turn off: energized = NC open = lights OFF.

/*! @name Simulated daylight window (see withinDaylightWindow in SensorPolicy.h)
 *  @details Outside [START, END) the lights stay off even in the dark, so as not
 *           to keep the plant lit 24 hours a day. Compile-time values (not
 *           in EEPROM): to make them tunable from the CLI they would need to move to DeviceConfig.
 *  @{ */
#define DAYLIGHT_START_HOUR  6   ///< Start hour of the daylight window, inclusive (06:00).
#define DAYLIGHT_END_HOUR    20  ///< End hour of the daylight window, exclusive (20:00).
/*! @} */

/*! @brief Physically turns on the lights (relay at rest, see polarity note above). */
static inline void lightsOn() {
    digitalWrite(GROWLIGHT_RELAY_PIN, GROWLIGHT_GPIO_LEVEL_ON);
}

/*! @brief Physically turns off the lights (relay energized, see polarity note above). */
static inline void lightsOff() {
    digitalWrite(GROWLIGHT_RELAY_PIN, GROWLIGHT_GPIO_LEVEL_OFF);
}

GrowLight::GrowLight() : _isOn(false) {}

void GrowLight::init() {
    // Safe rest state at startup: lights off until update() evaluates
    // the real policy (which already happens on the first loop iteration).
    // Same anti-glitch order as Pump::init(): level set before
    // switching the pin to OUTPUT.
    lightsOff();
    pinMode(GROWLIGHT_RELAY_PIN, OUTPUT);
    _isOn = false;
}

void GrowLight::update(CppMode targetMode, int lux, uint16_t threshold,
                       bool timeValid, uint32_t epochS) {
    uint8_t hourOfDay = timeValid ? (uint8_t)hour((time_t)epochS) : 0;
    bool daylight = withinDaylightWindow(timeValid, hourOfDay,
                                         DAYLIGHT_START_HOUR, DAYLIGHT_END_HOUR);
    bool want = daylight &&
               growLightWanted(targetMode == CPP_IDLE, lux, (int)threshold, _isOn);
    if (want == _isOn) return;
    if (want) lightsOn();
    else      lightsOff();
    _isOn = want;
}
