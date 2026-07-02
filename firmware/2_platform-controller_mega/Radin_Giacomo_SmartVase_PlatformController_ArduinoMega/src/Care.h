/*!
    @file   Care.h

    @ingroup MegaCare

    @brief  Autonomous plant-care module (homeostasis layer L2).

    @details Declares the ::Care class, the stateful counterpart of the pure
             decision logic in CarePolicy.h: CarePolicy.h decides, this module
             keeps the daily accounting (light budget, KPI counters, watering
             cycle, manual-override suspension) and talks to the actuators.
             Design rationale and decision tables: docs/Plant_Care_Design.md.

    @date   2026-07-01

    @author Giacomo Radin
*/

#ifndef CARE_H
#define CARE_H

#include <Arduino.h>
#include "smartvase_aliases.h"
#include "CarePolicy.h"

class Movement;
class Sensors;
class Pump;
class Persistence;
class Communication;
struct SystemStatus;

/*!
    @defgroup MegaCare Autonomous plant care (Mega)
    @brief  Homeostasis layer: decides when and why the movement, pump and grow
            light primitives are used, keeping the plant's light budget and soil
            moisture inside the range of the active plant profile.
    @{
*/

/*!
    @class Care
    @brief Non-blocking autonomous care state machine (the robot's day).

    @details Runs the pure decision logic of `CarePolicy.h` once per second and
             applies it to the actuators:
             - keeps the daily **light budget** accounting (relative DLI proxy,
               see ::CareBudget) and drives light/shadow seeking accordingly,
               starting a rotating light scan (Movement::startLightScan) on
               every relocation;
             - runs the **dose/soak/verify** irrigation cycle on top of Pump,
               under the existing tank/duration safety guards;
             - drives the UVA grow lights as an **end-of-day budget top-up**
               (GrowLight::force via the main loop) with a daily cap.

             Layer discipline (docs/Plant_Care_Design.md §2): this module only
             *requests* actions from L1 primitives; every L0 safety (degraded
             mode, tank protection, pump caps, motor timeouts, Hub deadman)
             stays in charge below it and is never bypassed.

             Disabled by default (`DeviceConfig::care_enabled == 0`): a freshly
             flashed robot never moves or waters on its own until `care on` is
             issued from the CLI. A manual mode change (CLI/Hub `setMode`)
             while care is active suspends the care layer for a grace period
             instead of fighting the operator.
*/
class Care {
public:
    /*! @brief Constructor: care idle, empty budget, no cycle in progress. */
    Care();

    /*!
        @brief    One care tick: accounting + decision + actuation (1 Hz internally).
        @details  To be called every main-loop iteration; internally throttled to
                  one evaluation per second. When care is disabled or suspended
                  (manual override) it keeps the light-budget accounting but does
                  not touch any actuator.
        @param[in,out] mv   Movement module (target mode, light scan, state).
        @param[in,out] sn   Sensors module (lux, soil, tank, clock).
        @param[in,out] pp   Pump module (irrigation doses).
        @param[in,out] ps   Persistence (active configuration + statistics).
        @param[in,out] comm Communication (log events toward the Hub).
        @param[in,out] sys  Shared system state (degraded mode, device id).
    */
    void tick(Movement& mv, Sensors& sn, Pump& pp, Persistence& ps,
              Communication& comm, SystemStatus& sys);

    /*!
        @brief    Notifies the module that `care_enabled` changed from the CLI.
        @details  On enable: resets the day accounting and restarts from
                  CARE_NIGHT (the next tick takes the "morning" decision). On
                  disable: releases the actuators (target mode back to IDLE,
                  grow-light demand cleared).
        @param[in,out] mv Movement module (released on disable).
        @param[in]     ps Persistence (reads the new `care_enabled`).
    */
    void notifyEnabledChanged(Movement& mv, Persistence& ps);

    /*! @brief true if care is enabled and not suspended by a manual override. */
    bool isActive() const { return _active; }

    /*! @brief Desired UVA grow-light state (meaningful only while isActive()). */
    bool growLightWanted() const { return _growLightWanted; }

    /*! @brief Current ::CareState code (for CLI/telemetry). */
    uint8_t stateCode() const { return _state; }

    /*! @brief Flash-string name of the current care state (for the CLI). */
    const __FlashStringHelper* stateName() const;

    /*! @brief Daily light budget achieved so far, percent of the profile target (KPI). */
    float budgetPct(const DeviceConfig& c) const {
        return careBudgetPct(_budget, c.care_light_target_min);
    }

    /*! @brief Current auto-calibration reference (max LDR ADC observed, decayed daily). */
    float dayMaxAdc() const { return _budget.day_max_adc; }

    /*! @brief Irrigation doses delivered today (KPI). */
    uint8_t dosesToday() const { return _dosesToday; }

    /*! @brief Seeking relocations started today (KPI). */
    uint8_t relocationsToday() const { return _relocationsToday; }

    /*! @brief UVA top-up minutes consumed today (KPI). */
    uint16_t growLightMinutesToday() const {
        return (uint16_t)(_growLightSecondsToday / 60.0f);
    }

    /*! @brief true while a dose/soak/verify watering cycle is in progress. */
    bool doseCycleActive() const { return _doseCycleActive; }

    /*! @brief Seconds left in the current soak wait (0 = no wait pending). */
    uint32_t soakRemainingS() const;

    /*! @brief true while care is suspended by a manual override (CLI/Hub setMode). */
    bool overrideActive() const { return _overrideUntilMs != 0; }

private:
    /*! @brief Builds the active PlantProfile from the persisted configuration. */
    PlantProfile profileFromConfig(const DeviceConfig& c) const;

    /*! @brief Handles the day rollover: KPI summary log + daily counters reset. */
    void handleDayChange(uint32_t epochS, Communication& comm,
                         Persistence& ps, SystemStatus& sys);

    /*! @brief Applies one careStep() output to the actuators and internal state. */
    void applyOutputs(const CareOutputs& o, int luxAdc, Movement& mv,
                      Persistence& ps, Communication& comm, SystemStatus& sys);

    /*! @brief Runs the dose/soak/verify watering cycle (stationary daylight states only). */
    void handleWatering(bool day, Sensors& sn, Pump& pp, Persistence& ps,
                        Communication& comm, SystemStatus& sys);

    uint8_t       _state;                 /**< Current ::CareState. */
    bool          _active;                /**< enabled && not suspended (cached each tick). */
    bool          _growLightWanted;       /**< Desired UVA state for the main loop. */
    CareBudget    _budget;                /**< Daily light-budget accumulator. */
    unsigned long _lastTickMs;            /**< Timestamp of the last 1 Hz evaluation. */
    uint32_t      _lastDayIndex;          /**< epoch/86400 of the last daily reset (0 = not anchored yet). */

    // Daily KPI counters
    uint8_t       _dosesToday;            /**< Irrigation doses delivered today. */
    uint8_t       _relocationsToday;      /**< Seeking sessions started today. */
    float         _growLightSecondsToday; /**< UVA top-up seconds consumed today. */

    // Watering cycle (dose / soak / verify)
    bool          _doseCycleActive;       /**< true between cycle start (soil dry) and wet target/cap. */
    unsigned long _soakUntilMs;           /**< End of the current absorption wait (0 = none). */
    bool          _tankWarned;            /**< One-shot latch for the tank-empty warning log. */

    // Seeking progress (gradient climb supervision)
    int           _bestSeekAdc;           /**< Best ADC seen in the current seeking session (-1 = none). */
    unsigned long _seekLastImproveMs;     /**< Timestamp of the last real ADC improvement while seeking. */
    unsigned long _seekStartMs;           /**< Timestamp of the current seeking session start. */

    // Basking spot reference (re-seek trigger)
    int           _bestSpotAdc;           /**< Best ADC seen at the current basking spot (-1 = none). */
    unsigned long _lowLightSinceMs;       /**< Start of the current low-light streak at the spot (0 = none). */
    unsigned long _heatSinceMs;           /**< Start of the current over-light streak (0 = none). */

    // Manual override (operator priority)
    CppMode       _lastCommandedMode;     /**< Last mode this module commanded to Movement. */
    bool          _modeCommanded;         /**< true once _lastCommandedMode is meaningful. */
    unsigned long _overrideUntilMs;       /**< Suspension deadline after a manual mode change (0 = none). */
};

/*! @} */ // MegaCare

#endif // CARE_H
