/*!
    @file   Care.cpp

    @ingroup MegaCare

    @brief  Implementation of the autonomous plant-care module (see Care.h).

    @details All *decisions* live in the pure functions of CarePolicy.h (host
             unit-tested in `tests/host/test_care_policy.cpp`); this file only
             gathers the inputs snapshot once per second, applies the outputs
             to Movement/Pump/GrowLight and keeps the daily accounting.
             Everything is millis()-based and non-blocking, per the project
             rule of no `delay()` in the main loop.

    @date   2026-07-01

    @author Giacomo Radin
*/

#include "Care.h"
#include "Movement.h"
#include "Sensors.h"
#include "Pump.h"
#include "Persistence.h"
#include "Communication.h"
#include "SystemStatus.h"
#include <TimeLib.h>

/*! @name Care scheduling constants
 *  @{ */
/*! @brief Care evaluation period (ms): the decision layer runs at 1 Hz. */
#define CARE_TICK_MS             1000UL
/*! @brief Upper clamp on the integration step (s): protects the light budget
           from a spurious huge dt after a re-enable or a millis() hiccup. */
#define CARE_DT_MAX_S            5.0f
/*! @brief Care suspension after a manual mode change (CLI/Hub): the operator
           keeps control for this long before autonomy resumes. */
#define CARE_OVERRIDE_SUSPEND_MS (30UL * 60UL * 1000UL)
/*! @} */

/*! @name Daylight window of the care cycle
 *  @details Same values as the legacy grow-light window in GrowLight.cpp: the
 *           care layer anchors its whole day (budget reset, night rest, top-up
 *           deadline) to this window. Compile-time for now; move to
 *           DeviceConfig if it ever needs CLI tuning.
 *  @{ */
#define CARE_DAY_START_HOUR  6   ///< Daylight window start, inclusive (06:00).
#define CARE_DAY_END_HOUR    20  ///< Daylight window end, exclusive (20:00).
/*! @} */

Care::Care()
    : _state(CARE_NIGHT),
      _active(false),
      _growLightWanted(false),
      _budget(careBudgetInit()),
      _lastTickMs(0),
      _lastDayIndex(0),
      _dosesToday(0),
      _relocationsToday(0),
      _growLightSecondsToday(0.0f),
      _doseCycleActive(false),
      _soakUntilMs(0),
      _tankWarned(false),
      _bestSeekAdc(-1),
      _seekLastImproveMs(0),
      _seekStartMs(0),
      _bestSpotAdc(-1),
      _lowLightSinceMs(0),
      _heatSinceMs(0),
      _lastCommandedMode(CPP_IDLE),
      _modeCommanded(false),
      _overrideUntilMs(0)
{
}

const __FlashStringHelper* Care::stateName() const {
    switch (_state) {
        case CARE_SEEK_SUN:   return F("SEEK_SUN");
        case CARE_BASK:       return F("BASK");
        case CARE_SEEK_SHADE: return F("SEEK_SHADE");
        case CARE_SHELTER:    return F("SHELTER");
        case CARE_TOP_UP:     return F("TOP_UP");
        case CARE_NIGHT:
        default:              return F("NIGHT");
    }
}

uint32_t Care::soakRemainingS() const {
    if (_soakUntilMs == 0) return 0;
    const unsigned long now = millis();
    if ((long)(now - _soakUntilMs) >= 0) return 0;
    return (uint32_t)(_soakUntilMs - now) / 1000UL;
}

PlantProfile Care::profileFromConfig(const DeviceConfig& c) const {
    PlantProfile p;
    p.light_target_min   = c.care_light_target_min;
    p.lux_high_adc       = c.care_lux_high_adc;
    p.soil_dry_adc       = c.soil_dry_threshold;   // single source: the existing dry threshold
    p.soil_wet_adc       = c.care_soil_wet_adc;
    p.dose_ms            = c.care_dose_ms;
    p.soak_min           = c.care_soak_min;
    p.max_doses_per_day  = c.care_max_doses;
    p.max_reloc_per_day  = c.care_max_reloc;
    p.grow_light_max_min = c.care_growlight_max_min;
    return p;
}

void Care::notifyEnabledChanged(Movement& mv, Persistence& ps) {
    const bool enabled = ps.getConfig().care_enabled != 0;
    // Fresh start in both directions: day accounting restarts from scratch and
    // the state machine re-decides from NIGHT on the next tick.
    _state                 = CARE_NIGHT;
    _budget                = careBudgetInit();
    _lastDayIndex          = 0;
    _dosesToday            = 0;
    _relocationsToday      = 0;
    _growLightSecondsToday = 0.0f;
    _doseCycleActive       = false;
    _soakUntilMs           = 0;
    _tankWarned            = false;
    _bestSeekAdc           = -1;
    _bestSpotAdc           = -1;
    _lowLightSinceMs       = 0;
    _heatSinceMs           = 0;
    _overrideUntilMs       = 0;
    _modeCommanded         = false;
    _growLightWanted       = false;
    _lastTickMs            = millis();
    _active                = enabled;
    if (!enabled) {
        // Release the actuators we may have been driving.
        mv.setTargetMode(CPP_IDLE);
    }
}

void Care::handleDayChange(uint32_t epochS, Communication& comm,
                           Persistence& ps, SystemStatus& sys) {
    const uint32_t dayIdx = epochS / 86400UL;
    if (dayIdx == _lastDayIndex) return;
    if (_lastDayIndex != 0) {
        // End-of-day KPI summary toward the Hub/cloud (budget %, doses,
        // relocations, UVA minutes) — the measurable side of the care layer.
        char detail[24];
        const int pct = (int)budgetPct(ps.getConfig());
        snprintf(detail, sizeof(detail), "b%d d%u r%u g%u",
                 pct, (unsigned)_dosesToday, (unsigned)_relocationsToday,
                 (unsigned)growLightMinutesToday());
        comm.logEvent(Log_LogLevel_INFO, "care_day_end", detail,
                      sys.deviceId, ps.getStats());
    }
    _lastDayIndex = dayIdx;
    careBudgetDailyReset(_budget);
    _dosesToday            = 0;
    _relocationsToday      = 0;
    _growLightSecondsToday = 0.0f;
    _doseCycleActive       = false;
    _soakUntilMs           = 0;
    _tankWarned            = false;
}

void Care::applyOutputs(const CareOutputs& o, int luxAdc, Movement& mv,
                        Persistence& ps, Communication& comm, SystemStatus& sys) {
    // State transition bookkeeping.
    if (o.next != _state) {
        if (o.next == CARE_SEEK_SUN || o.next == CARE_SEEK_SHADE) {
            _relocationsToday++;
            _bestSeekAdc       = -1;
            _seekLastImproveMs = millis();
            _seekStartMs       = millis();
        }
        if (o.next == CARE_BASK) {
            _bestSpotAdc     = (luxAdc >= 0) ? luxAdc : -1;
            _lowLightSinceMs = 0;
        }
        _state = o.next;
        char detail[16];
        strncpy_P(detail, (const char*)stateName(), sizeof(detail) - 1);
        detail[sizeof(detail) - 1] = '\0';
        comm.logEvent(Log_LogLevel_INFO, "care_state", detail,
                      sys.deviceId, ps.getStats());
    }

    // Movement mode (only touch it when it differs: keeps the override
    // detection in tick() meaningful).
    CppMode want = CPP_IDLE;
    if (o.mode == CARE_MODE_LIGHT)  want = CPP_LIGHT;
    if (o.mode == CARE_MODE_SHADOW) want = CPP_SHADOW;
    if (mv.getTargetMode() != want) mv.setTargetMode(want);
    _lastCommandedMode = want;
    _modeCommanded     = true;

    if (o.startSeekScan) {
        mv.startLightScan(o.mode == CARE_MODE_LIGHT, ps.getStats());
    }

    _growLightWanted = o.growLightOn;
}

void Care::handleWatering(bool day, Sensors& sn, Pump& pp, Persistence& ps,
                          Communication& comm, SystemStatus& sys) {
    // Watering runs only while stationary during the day: dosing while driving
    // would spill, and night irrigation promotes fungus (horticultural practice).
    const bool stationary = (_state == CARE_BASK || _state == CARE_SHELTER ||
                             _state == CARE_TOP_UP);
    if (!day || !stationary) return;
    if (pp.isActive()) return;

    const unsigned long now = millis();
    if (_soakUntilMs != 0 && (long)(now - _soakUntilMs) < 0) return;  // absorbing

    const DeviceConfig& c = ps.getConfig();
    const PlantProfile  p = profileFromConfig(c);
    const int soil = sn.getSoilMoisture();

    // Cycle end: wet target reached after the soak wait.
    if (_doseCycleActive && soil >= 0 && soil >= (int)p.soil_wet_adc) {
        _doseCycleActive = false;
        _soakUntilMs     = 0;
        comm.logEvent(Log_LogLevel_INFO, "care_water", "cycle_done",
                      sys.deviceId, ps.getStats());
        return;
    }

    if (!careDoseWanted(soil, p.soil_dry_adc, p.soil_wet_adc, _doseCycleActive)) return;

    // Daily cap: fail-safe against a stuck-dry (broken/unplugged) soil probe.
    if (_dosesToday >= p.max_doses_per_day) {
        if (_doseCycleActive) {
            _doseCycleActive = false;
            _soakUntilMs     = 0;
            comm.logEvent(Log_LogLevel_WARN, "care_water", "dose_cap",
                          sys.deviceId, ps.getStats());
        }
        return;
    }

    // Tank guard (same fail-safe as the manual paths; the main loop also stops
    // a running dose if the tank empties mid-way).
    if (sn.tankLooksEmpty(c.tank_empty_cm)) {
        if (!_tankWarned) {
            _tankWarned = true;
            comm.logEvent(Log_LogLevel_WARN, "care_water", "tank_empty",
                          sys.deviceId, ps.getStats());
        }
        return;
    }
    _tankWarned = false;

    if (pp.start((uint32_t)p.dose_ms, ps.getStats())) {
        _doseCycleActive = true;
        _dosesToday++;
        _soakUntilMs = now + (unsigned long)p.soak_min * 60000UL;
        comm.logEvent(Log_LogLevel_INFO, "care_water", "dose",
                      sys.deviceId, ps.getStats());
    }
}

void Care::tick(Movement& mv, Sensors& sn, Pump& pp, Persistence& ps,
                Communication& comm, SystemStatus& sys) {
    const DeviceConfig& c = ps.getConfig();

    if (c.care_enabled == 0) {
        // Disabled: fully passive (legacy behavior everywhere). Keep the tick
        // anchor fresh so a later enable does not integrate a giant dt.
        _active          = false;
        _growLightWanted = false;
        _lastTickMs      = millis();
        return;
    }

    const unsigned long now = millis();
    if (now - _lastTickMs < CARE_TICK_MS) return;
    float dt = (float)(now - _lastTickMs) / 1000.0f;
    if (dt > CARE_DT_MAX_S) dt = CARE_DT_MAX_S;
    _lastTickMs = now;

    // --- Clock / calendar ---
    const bool     timeValid = sn.timeIsValid();
    const uint32_t epochS    = timeValid ? sn.getEpoch() : 0;
    const uint8_t  hourNow   = timeValid ? (uint8_t)hour((time_t)epochS) : 0;
    const uint8_t  minNow    = timeValid ? (uint8_t)minute((time_t)epochS) : 0;
    if (timeValid) handleDayChange(epochS, comm, ps, sys);

    const bool day = timeValid &&
                     hourNow >= CARE_DAY_START_HOUR && hourNow < CARE_DAY_END_HOUR;

    // --- Accounting (always, even while suspended: the light keeps shining) ---
    const int lux = sn.getLux();
    if (day) careBudgetUpdate(_budget, lux, dt);

    const PlantProfile p = profileFromConfig(c);

    // Over-light (heat proxy) streak.
    if (lux >= 0 && lux > (int)p.lux_high_adc) {
        if (_heatSinceMs == 0) _heatSinceMs = now;
    } else {
        _heatSinceMs = 0;
    }

    // Low-light streak at the basking spot (re-seek trigger).
    if (_state == CARE_BASK) {
        if (lux > _bestSpotAdc) _bestSpotAdc = lux;
        if (_bestSpotAdc > 0 && lux >= 0 &&
            (float)lux < CARE_RESEEK_FRAC * (float)_bestSpotAdc) {
            if (_lowLightSinceMs == 0) _lowLightSinceMs = now;
        } else {
            _lowLightSinceMs = 0;
        }
    } else {
        _lowLightSinceMs = 0;
    }

    // Seeking progress supervision (gradient climb stall detection).
    bool atLocalMax = false, seekTimedOut = false;
    if (_state == CARE_SEEK_SUN || _state == CARE_SEEK_SHADE) {
        if (lux >= 0) {
            const bool improved =
                (_bestSeekAdc < 0) ||
                (_state == CARE_SEEK_SUN
                     ? (lux >= _bestSeekAdc + CARE_IMPROVE_MIN_ADC)
                     : (lux <= _bestSeekAdc - CARE_IMPROVE_MIN_ADC));
            if (improved) {
                _bestSeekAdc       = lux;
                _seekLastImproveMs = now;
            }
        }
        // The stall clock only runs while actually driving: scan phases and
        // avoidance recoveries do not count as "no progress".
        if (mv.getCurrentState() != CPP_M_MOVING) _seekLastImproveMs = now;
        if (now - _seekLastImproveMs > (unsigned long)CARE_STALL_S * 1000UL)
            atLocalMax = true;
        if (now - _seekStartMs > (unsigned long)CARE_SEEK_TIMEOUT_S * 1000UL)
            seekTimedOut = true;
    }

    // --- Manual override: the operator always wins ---
    if (_modeCommanded && mv.getTargetMode() != _lastCommandedMode) {
        _overrideUntilMs = now + CARE_OVERRIDE_SUSPEND_MS;
        if (_overrideUntilMs == 0) _overrideUntilMs = 1;   // 0 means "none"
        _modeCommanded = false;
        comm.logEvent(Log_LogLevel_INFO, "care_paused", "manual_override",
                      sys.deviceId, ps.getStats());
    }
    if (_overrideUntilMs != 0) {
        if ((long)(now - _overrideUntilMs) < 0) {
            _active          = false;   // legacy grow-light rule applies meanwhile
            _growLightWanted = false;
            return;
        }
        _overrideUntilMs = 0;
        comm.logEvent(Log_LogLevel_INFO, "care_resumed", "override_expired",
                      sys.deviceId, ps.getStats());
    }
    _active = true;

    // --- Degraded mode: L0 owns the robot; care waits without acting ---
    if (sys.degradedModeActive) {
        _growLightWanted = false;
        return;
    }

    // --- Decision (pure) + actuation ---
    CareInputs in;
    in.timeValid             = timeValid;
    in.hourOfDay             = hourNow;
    in.dayStartHour          = CARE_DAY_START_HOUR;
    in.dayEndHour            = CARE_DAY_END_HOUR;
    in.budgetPct             = careBudgetPct(_budget, p.light_target_min);
    in.luxAdc                = lux;
    in.heatStreakS           = (_heatSinceMs == 0)
                                   ? 0 : (uint16_t)((now - _heatSinceMs) / 1000UL);
    in.lowLightStreakS       = (_lowLightSinceMs == 0)
                                   ? 0 : (uint16_t)((now - _lowLightSinceMs) / 1000UL);
    in.relocationsToday      = _relocationsToday;
    in.minutesToWindowEnd    = day
        ? (uint16_t)((CARE_DAY_END_HOUR * 60) - (hourNow * 60 + minNow)) : 0;
    const uint16_t glMin     = growLightMinutesToday();
    in.growLightMinutesToday = (glMin > 255) ? 255 : (uint8_t)glMin;
    in.atLocalMax            = atLocalMax;
    in.seekTimedOut          = seekTimedOut;

    const CareOutputs o = careStep(_state, in, p);
    applyOutputs(o, lux, mv, ps, comm, sys);

    if (_growLightWanted) _growLightSecondsToday += dt;

    // --- Watering (dose / soak / verify) ---
    handleWatering(day, sn, pp, ps, comm, sys);
}
