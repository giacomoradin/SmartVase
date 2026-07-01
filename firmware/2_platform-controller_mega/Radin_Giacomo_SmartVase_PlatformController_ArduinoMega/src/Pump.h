/*!
    @file   Pump.h

    @ingroup MegaPump

    @brief  Non-blocking control of the irrigation pump relay.

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef PUMP_H
#define PUMP_H

#include <Arduino.h>
#include "smartvase_aliases.h"

/*!
    @addtogroup MegaPump
    @{
*/

/*!
    @class Pump
    @brief Non-blocking module for controlling the irrigation pump relay.

    @details Lets irrigation be started for a programmed duration and turns the
             pump off automatically when the time elapses, via polling in the main
             loop (`tick()`, no `delay()`). Updates the cumulative irrigation
             statistics (count and total duration). The relay polarity handling
             (active low/high) and the safety cap on the duration live in the
             implementation (see Pump.cpp).
*/
class Pump {
public:
    /**
     * @brief Constructs the Pump object.
     */
    Pump();

    /**
     * @brief Initializes the pump relay pin, setting it as an output in the off state.
     */
    void init();

    /**
     * @brief Starts watering the plant for the specified duration.
     *
     * @param duration_ms Irrigation duration in milliseconds.
     * @param stats Reference to the cumulative statistics, to record the activation.
     * @return true if irrigation was started successfully, false if the pump was already active.
     */
    bool start(uint32_t duration_ms, CumulativeStats& stats);

    /**
     * @brief Forces an immediate pump stop.
     *
     * Also to be called on emergency or in degraded mode.
     *
     * @param stats Reference to the cumulative statistics, to update the counters.
     */
    void stop(CumulativeStats& stats);

    /**
     * @brief Manages the irrigation timer and performs the automatic stop when it expires.
     *
     * To be called at high frequency in the main loop.
     *
     * @param stats Reference to the cumulative statistics.
     */
    void tick(CumulativeStats& stats);

    /**
     * @brief Returns whether the pump is currently active.
     * @return true if active, false otherwise.
     */
    bool isActive() const { return active; }

private:
    bool          active;             /**< Relay activity state. */
    unsigned long start_ms;           /**< Timestamp (in ms) of the pump start. */
    uint32_t      duration_ms_target; /**< Target duration of the current irrigation (in ms). */
};

/*! @} */ // MegaPump

#endif // PUMP_H
