/*!
    @file   GrowLight.h

    @ingroup MegaGrowLight

    @brief  Non-blocking control of the grow light (UVA) relay.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef GROW_LIGHT_H
#define GROW_LIGHT_H

#include <Arduino.h>
#include "smartvase_aliases.h"

/*!
    @defgroup MegaGrowLight Grow lights (Mega)
    @brief  Channel 2 relay (D11) driving the UVA grow lights, automatically
            turned on when the robot is IDLE and the ambient light is insufficient.
    @{
*/

/*!
    @class GrowLight
    @brief Non-blocking module for controlling the UVA grow light relay.

    @details The lights are wired on the **NC** (Normally Closed) contact of the
             relay module's second channel (see GrowLight.cpp for the polarity
             details): unlike the pump, here "light on" corresponds to the
             relay being **at rest** (not energized). The decision on
             when to turn them on is delegated to two pure functions in
             `SensorPolicy.h`, so the policy remains host-testable
             independently of the hardware access:
             - `growLightWanted()` — IDLE mode + insufficient ambient light;
             - `withinDaylightWindow()` — simulates the solar day/night cycle
               (06:00-20:00 by default, see `DAYLIGHT_START_HOUR`/`DAYLIGHT_END_HOUR`
               in GrowLight.cpp): outside this window, or if the RTC time
               is not reliable, the lights stay off even in the dark.

             When the autonomous care layer is active (Care.cpp, `care on`),
             the main loop bypasses this legacy policy and drives the relay
             directly through force(): the lights then become the end-of-day
             light-budget top-up decided by the care state machine
             (CARE_TOP_UP, see CarePolicy.h), with its own daily cap.
*/
class GrowLight {
public:
    /*! @brief Constructor: initial state off (the pin is not configured yet). */
    GrowLight();

    /*!
        @brief    Configures the relay pin and forces the lights to the safe rest state (off).
        @note     To be called once in `setup()`, after the other modules have been initialized.
    */
    void init();

    /*!
        @brief    Evaluates the policy (mode + light + daylight window) and updates the relay if the state changes.
        @details  To be called at high frequency in the main loop (non-blocking). Writes the pin only
                  when the desired state differs from the current one, so as not to "chatter" the
                  relay on every iteration. The daylight window (see `withinDaylightWindow()`) is
                  evaluated first: if we are outside the time range or the RTC is not reliable, the lights
                  stay off regardless of mode/light.
        @param[in] targetMode Current target mode of the robot (Movement::getTargetMode()).
        @param[in] lux        Current light ADC value (negative = not valid).
        @param[in] threshold  Configured light ADC threshold (`light_threshold`).
        @param[in] timeValid  `true` if the RTC has a reliable time (Sensors::getRTCStatus() && !Sensors::rtcOscStopped()).
        @param[in] epochS     Current UNIX epoch (Sensors::getEpoch()), used to extract the hour of day.
    */
    void update(CppMode targetMode, int lux, uint16_t threshold,
               bool timeValid, uint32_t epochS);

    /*!
        @brief    Directly sets the lights state, bypassing the legacy policy.
        @details  Used when the autonomous care layer (Care.cpp) is active: the
                  decision of when to turn the UVA lights on then belongs to the
                  care state machine (end-of-day budget top-up, see CarePolicy.h)
                  instead of the simple "IDLE + dark + daylight window" rule of
                  update(). Writes the relay pin only on a state change, like
                  update() (no relay chatter).
        @param[in] on Desired lights state.
    */
    void force(bool on);

    /*! @brief Returns whether the lights are currently on. */
    bool isOn() const { return _isOn; }

private:
    bool _isOn; /**< Current logical state of the lights (true = on). */
};

/*! @} */ // MegaGrowLight

#endif // GROW_LIGHT_H
