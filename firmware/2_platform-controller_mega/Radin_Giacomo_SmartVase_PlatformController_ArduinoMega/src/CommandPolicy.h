/*!
    @file   CommandPolicy.h

    @ingroup MegaPolicy

    @brief  Pure validation/clamp policies for the commands received from the Hub.

    @details `static inline` functions with no hardware dependency (no
             `Arduino.h`), meant to be included both by the firmware
             (`Communication.cpp`, during `executeCommand`) and by the host
             unit tests in `tests/host/` (see `test_command_policy`).
             They implement the Mega-side defense-in-depth: even though the
             upstream Hub already validates/clamps the commands, the Mega does
             not trust it blindly and re-applies the same constraints before
             acting on motors/pump/EEPROM.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef COMMAND_POLICY_H
#define COMMAND_POLICY_H

#include <stdint.h>

/*!
    @addtogroup MegaPolicy
    @{
*/

/*!
    @brief    Decides whether a `water` command can be accepted.

    @details  Watering is allowed only if at least `minIntervalMs` has elapsed
              since the last accepted one: anti over-watering and anti-flood,
              in addition to the duration cap (60 s) and the refusal when the
              pump is already active (handled elsewhere). The arithmetic is
              unsigned `uint32_t`, so it is wraparound-safe with respect to the
              `millis()` rollover (~49 days).

    @param[in] nowMs          Current timestamp (`millis()`).
    @param[in] lastAcceptedMs Timestamp of the last accepted watering;
                               `0` means "none accepted yet".
    @param[in] minIntervalMs  Minimum required interval between two waterings, in ms.

    @return   `true` if the command can be executed, `false` if it must be
              rejected for rate-limiting.

    @note     If `lastAcceptedMs == 0` the first watering is always allowed.
*/
static inline bool waterAllowed(uint32_t nowMs, uint32_t lastAcceptedMs,
                                uint32_t minIntervalMs) {
    if (lastAcceptedMs == 0) return true;
    return (nowMs - lastAcceptedMs) >= minIntervalMs;
}

/*!
    @brief    Determines whether the motion parameters have actually changed.

    @details  Used before a `setMotionParams` to avoid EEPROM writes when the
              requested value is identical to the one already persisted
              (cell wear protection: the EEPROM has a finite number of write
              cycles).

    @param[in] curRev  Avoidance reverse duration currently stored (ms).
    @param[in] curTurn Avoidance turn duration currently stored (ms).
    @param[in] newRev  New requested reverse duration (ms).
    @param[in] newTurn New requested turn duration (ms).

    @return   `true` if at least one of the two parameters differs from the current value.
*/
static inline bool motionParamsChanged(uint16_t curRev, uint16_t curTurn,
                                       uint16_t newRev, uint16_t newTurn) {
    return (curRev != newRev) || (curTurn != newTurn);
}

/*!
    @brief    Clamps the requested pump activation duration.

    @details  Defense-in-depth: the Hub already limits the duration on its side,
              but the Mega re-applies the clamp anyway to protect plant and pump
              from malformed commands or from a compromised Hub.

    @param[in] ms    Requested irrigation duration, in ms.
    @param[in] maxMs Maximum allowed safety duration, in ms.

    @return   `ms` if within the limit, otherwise `maxMs`.
*/
static inline uint32_t clampWaterDurationMs(uint32_t ms, uint32_t maxMs) {
    return (ms > maxMs) ? maxMs : ms;
}

/*!
    @brief    Clamps a motion parameter (reverse/turn) to a safe range.

    @param[in] ms Requested value, in ms.
    @param[in] lo Lowest allowed bound, in ms.
    @param[in] hi Highest allowed bound, in ms.

    @return   `ms` clamped to the interval `[lo, hi]`.
*/
static inline uint16_t clampMotionParamMs(uint32_t ms, uint16_t lo, uint16_t hi) {
    if (ms < (uint32_t)lo) return lo;
    if (ms > (uint32_t)hi) return hi;
    return (uint16_t)ms;
}

/*! @} */ // MegaPolicy

#endif // COMMAND_POLICY_H
