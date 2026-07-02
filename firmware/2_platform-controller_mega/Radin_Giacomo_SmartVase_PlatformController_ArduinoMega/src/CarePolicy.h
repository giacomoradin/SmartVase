/*!
    @file   CarePolicy.h

    @ingroup MegaPolicy

    @brief  Pure autonomous plant-care policy (homeostasis layer, L2).

    @details `static inline` functions with no hardware dependency (only
             `<stdint.h>`/`<math.h>`), meant to be included both by the firmware
             (`Care.cpp`, `Movement.cpp`) and by the host unit tests in
             `tests/host/` (see `test_care_policy`). Together they implement
             the behavioral layer that decides *when and why* the existing
             primitives (seeking, pump, grow lights) are used, based on a
             per-plant profile and a daily light budget:

             - `PlantProfile` + `carePresetProfile()` — per-plant setpoints
               (light target, soil hysteresis band, dose sizing, daily caps);
             - `CareBudget` + `careBudget*()` — daily light-budget accounting:
               the uncalibrated LDR ADC is normalized against the maximum
               value observed in the room (auto-calibration) and integrated
               over time into "full-light seconds", a relative proxy of the
               agronomic DLI (Daily Light Integral);
             - `careBestScanSector()` — sector selection for the rotating
               light scan (`lightScan` primitive in Movement.cpp);
             - `careDoseWanted()` — dose/soak/verify irrigation decision;
             - `careStep()` — the daily care state machine (CareState), a
               pure transition function implementing the decision table in
               `docs/Plant_Care_Design.md` §6.

             Fail-safe conventions, consistent with SensorPolicy.h: an invalid
             ADC reading (negative) never triggers an action, and without a
             reliable clock the care layer parks the robot (CARE_NIGHT).

    @date   2026-07-01

    @author Giacomo Radin
*/

#ifndef CARE_POLICY_H
#define CARE_POLICY_H

#include <stdint.h>
#include <math.h>   // isnan, NAN

/*!
    @addtogroup MegaPolicy
    @{
*/

// =================================================================
// Tuning constants (bench-tunable; keep pure — no hardware here)
// =================================================================

/*! @brief Number of angular sectors sampled by the rotating light scan. */
#define CARE_SCAN_SECTORS        12

/*! @brief Floor for the day-max ADC normalization: prevents a dark room from
           inflating the normalized light level (11/11 would read as "full sun"). */
#define CARE_LUX_FLOOR_ADC       300.0f

/*! @brief Decay applied to the day-max ADC at every daily reset, so the
           auto-calibration can also adapt downward across days (season change,
           robot moved to a darker room). Clamped to CARE_LUX_FLOOR_ADC. */
#define CARE_DAYMAX_DECAY        0.75f

/*! @brief Re-seek trigger: fraction of the best recent ADC below which the
           current spot is considered to have lost its light (cloud, sun moved). */
#define CARE_RESEEK_FRAC         0.70f

/*! @brief Re-seek trigger: seconds of continuous low light (see CARE_RESEEK_FRAC)
           required before relocating. Hysteresis against passing clouds. */
#define CARE_RESEEK_STREAK_S     600U

/*! @brief Heat-proxy trigger: seconds of continuous ADC above the profile's
           `lux_high_adc` before seeking shade (overheat protection proxy). */
#define CARE_HEAT_STREAK_S       900U

/*! @brief Grow-light top-up window: minutes before the daylight window closes
           within which the UVA top-up may start. */
#define CARE_TOPUP_START_MIN     60U

/*! @brief Grow-light top-up trigger: budget percentage below which the UVA
           top-up is considered worth running. */
#define CARE_TOPUP_BUDGET_PCT    80.0f

/*! @brief Budget percentage at/above which the daily light target is met. */
#define CARE_BUDGET_DONE_PCT     100.0f

/*! @brief Gradient-climb stall: seconds without ADC improvement after which
           the seeking is considered to have reached its local maximum. */
#define CARE_STALL_S             8U

/*! @brief Minimum ADC improvement considered a real gain during seeking
           (below this it is noise, not progress). */
#define CARE_IMPROVE_MIN_ADC     8

/*! @brief Hard timeout on a single seeking session (seconds): past this the
           care layer settles where it is (avoids endless wandering). */
#define CARE_SEEK_TIMEOUT_S      120U

// =================================================================
// Plant profile
// =================================================================

/*! @brief Preset plant kinds for carePresetProfile(). */
enum CarePlantKind : uint8_t {
    CARE_PLANT_SHADE  = 0,  /**< Low-light plant (e.g. pothos, ferns). */
    CARE_PLANT_MEDIUM = 1,  /**< Medium-light plant (default). */
    CARE_PLANT_SUN    = 2   /**< Full-sun plant (e.g. basil, succulents). */
};

/*!
    @struct PlantProfile
    @brief  Per-plant care setpoints (the "what this plant needs" contract).

    @details Soil ADC convention follows the existing firmware convention
             (`soil_dry_threshold` in DeviceConfig): LOWER ADC = drier soil.
             `soil_wet_adc > soil_dry_adc` forms the dose/verify hysteresis
             band. Light target is expressed in minutes of *equivalent full
             light* per day (relative DLI proxy, see CareBudget).
*/
struct PlantProfile {
    uint16_t light_target_min;   /**< Daily light target, in full-light minutes (see CareBudget). */
    uint16_t lux_high_adc;       /**< ADC above which prolonged exposure triggers shade seeking (1024 = never). */
    uint16_t soil_dry_adc;       /**< Below this ADC the soil is dry: start a watering cycle. */
    uint16_t soil_wet_adc;       /**< At/above this ADC the soil is wet enough: stop the cycle. */
    uint16_t dose_ms;            /**< Single irrigation dose duration (ms), well under the pump caps. */
    uint8_t  soak_min;           /**< Absorption wait between doses (minutes). */
    uint8_t  max_doses_per_day;  /**< Daily dose cap (fail-safe against a broken soil probe). */
    uint8_t  max_reloc_per_day;  /**< Daily relocation cap (energy proxy, anti-wandering). */
    uint8_t  grow_light_max_min; /**< Daily UVA top-up cap (minutes). */
};

/*!
    @brief    Returns the preset profile for a plant kind.

    @details  Values are conservative bench-starting points, meant to be tuned
              per plant from the CLI (`plant` command). Unknown kinds fall back
              to the MEDIUM preset (fail-safe default).

    @param[in] kind One of ::CarePlantKind.

    @return   The preset ::PlantProfile.
*/
static inline PlantProfile carePresetProfile(uint8_t kind) {
    PlantProfile p;
    switch (kind) {
        case CARE_PLANT_SHADE:
            p.light_target_min   = 120;   // ~2 h equivalent full light
            p.lux_high_adc       = 700;   // shade plants dislike direct sun
            p.soil_dry_adc       = 400;
            p.soil_wet_adc       = 520;
            p.dose_ms            = 3000;
            p.soak_min           = 30;
            p.max_doses_per_day  = 4;
            p.max_reloc_per_day  = 6;
            p.grow_light_max_min = 60;
            break;
        case CARE_PLANT_SUN:
            p.light_target_min   = 390;   // ~6.5 h
            p.lux_high_adc       = 1024;  // never shade for heat (ADC max is 1023)
            p.soil_dry_adc       = 480;
            p.soil_wet_adc       = 600;
            p.dose_ms            = 5000;
            p.soak_min           = 25;
            p.max_doses_per_day  = 8;
            p.max_reloc_per_day  = 8;
            p.grow_light_max_min = 180;
            break;
        case CARE_PLANT_MEDIUM:
        default:
            p.light_target_min   = 240;   // ~4 h
            p.lux_high_adc       = 850;
            p.soil_dry_adc       = 450;
            p.soil_wet_adc       = 570;
            p.dose_ms            = 4000;
            p.soak_min           = 30;
            p.max_doses_per_day  = 6;
            p.max_reloc_per_day  = 6;
            p.grow_light_max_min = 120;
            break;
    }
    return p;
}

// =================================================================
// Daily light budget (relative DLI proxy)
// =================================================================

/*!
    @struct CareBudget
    @brief  Daily light-budget accumulator state.

    @details The LDR is not calibrated in lux, but control only needs a
             *consistent* relative measure: every sample contributes
             `luxAdc / day_max_adc` (0..1, "fraction of the best light this
             room has shown") multiplied by the elapsed time. `day_max_adc`
             rises instantly to any new maximum and decays slowly at each
             daily reset, auto-calibrating the scale per room/season.
*/
struct CareBudget {
    float accum_s;      /**< Accumulated full-light-equivalent seconds today. */
    float day_max_adc;  /**< Reference maximum ADC (auto-calibration), >= CARE_LUX_FLOOR_ADC. */
};

/*!
    @brief  Returns a freshly initialized budget (empty, floor reference).
    @return A ::CareBudget with zero accumulation and `day_max_adc` at the floor.
*/
static inline CareBudget careBudgetInit() {
    CareBudget b;
    b.accum_s     = 0.0f;
    b.day_max_adc = CARE_LUX_FLOOR_ADC;
    return b;
}

/*!
    @brief    Daily reset of the budget: clears the accumulator, decays the reference max.
    @details  The decay (CARE_DAYMAX_DECAY) lets the auto-calibration adapt
              downward across days; the floor (CARE_LUX_FLOOR_ADC) prevents a
              dark room from ever reading as "full light".
    @param[in,out] b Budget state to reset.
*/
static inline void careBudgetDailyReset(CareBudget& b) {
    b.accum_s = 0.0f;
    b.day_max_adc *= CARE_DAYMAX_DECAY;
    if (b.day_max_adc < CARE_LUX_FLOOR_ADC) b.day_max_adc = CARE_LUX_FLOOR_ADC;
}

/*!
    @brief    Integrates one light sample into the daily budget.
    @details  Invalid readings (negative ADC) are ignored (fail-safe: no
              accumulation on blind sensor, consistent with SensorPolicy.h).
    @param[in,out] b      Budget state.
    @param[in]     luxAdc Current LDR ADC reading (negative = invalid).
    @param[in]     dt_s   Seconds elapsed since the previous sample.
*/
static inline void careBudgetUpdate(CareBudget& b, int luxAdc, float dt_s) {
    if (luxAdc < 0 || dt_s <= 0.0f) return;
    if ((float)luxAdc > b.day_max_adc) b.day_max_adc = (float)luxAdc;
    b.accum_s += ((float)luxAdc / b.day_max_adc) * dt_s;
}

/*!
    @brief    Percentage of the daily light target achieved so far.
    @param[in] b          Budget state.
    @param[in] target_min Daily target in full-light minutes (from PlantProfile).
    @return   0..100+ (may exceed 100 when the target is surpassed);
              100 if the target is zero/invalid (nothing to chase).
*/
static inline float careBudgetPct(const CareBudget& b, uint16_t target_min) {
    if (target_min == 0) return 100.0f;
    return (b.accum_s / ((float)target_min * 60.0f)) * 100.0f;
}

// =================================================================
// Light-scan sector selection (for the Movement lightScan primitive)
// =================================================================

/*!
    @brief    Picks the best angular sector from a rotating light scan.

    @details  During the scan the robot rotates in place and accumulates the
              mean LDR ADC per time sector; this selects the brightest sector
              when seeking light, the darkest when seeking shade. Sectors with
              no valid samples (NaN or negative mean) are skipped.

    @param[in] meanAdc   Array of per-sector mean ADC values (NaN/negative = no data).
    @param[in] n         Number of sectors (typically CARE_SCAN_SECTORS).
    @param[in] seekLight true = pick the maximum (light), false = the minimum (shade).

    @return   The best sector index (0..n-1), or -1 if no sector has valid data.
*/
static inline int8_t careBestScanSector(const float* meanAdc, uint8_t n, bool seekLight) {
    int8_t best = -1;
    float bestV = 0.0f;
    for (uint8_t i = 0; i < n; ++i) {
        const float v = meanAdc[i];
        if (isnan(v) || v < 0.0f) continue;
        if (best < 0 || (seekLight ? (v > bestV) : (v < bestV))) {
            best  = (int8_t)i;
            bestV = v;
        }
    }
    return best;
}

// =================================================================
// Irrigation: dose / soak / verify
// =================================================================

/*!
    @brief    Decides whether an irrigation dose should be delivered now.

    @details  Implements the hysteresis band of the dose/soak/verify cycle:
              a cycle *starts* only when the soil reads drier than
              `soil_dry_adc`, and once started it keeps dosing (one dose per
              soak period, managed by the caller) until the soil reaches
              `soil_wet_adc`. The caller enforces the orthogonal guards
              (tank level, daily dose cap, soak timer, pump not active,
              degraded mode). Invalid readings never start a dose:
              watering blind is worse than skipping a day.

    @param[in] soilAdc     Current soil ADC (negative = invalid). Lower = drier.
    @param[in] dryAdc      Cycle-start threshold (from PlantProfile).
    @param[in] wetAdc      Cycle-stop threshold (from PlantProfile, > dryAdc).
    @param[in] cycleActive true if a watering cycle is already in progress.

    @return   true if a dose should be delivered now.
*/
static inline bool careDoseWanted(int soilAdc, uint16_t dryAdc, uint16_t wetAdc,
                                  bool cycleActive) {
    if (soilAdc < 0) return false;
    if (!cycleActive) return soilAdc < (int)dryAdc;
    return soilAdc < (int)wetAdc;
}

// =================================================================
// Care state machine (the robot's day)
// =================================================================

/*! @brief States of the daily care cycle (see docs/Plant_Care_Design.md §6). */
enum CareState : uint8_t {
    CARE_NIGHT      = 0,  /**< Outside the daylight window (or no reliable clock): parked, lights off. */
    CARE_SEEK_SUN   = 1,  /**< Light scan + gradient climb toward the brightest reachable spot. */
    CARE_BASK       = 2,  /**< Stationary in good light, accumulating budget. */
    CARE_SEEK_SHADE = 3,  /**< Seeking a darker spot (budget met or heat proxy tripped). */
    CARE_SHELTER    = 4,  /**< Stationary in shade. */
    CARE_TOP_UP     = 5   /**< UVA grow lights compensating an end-of-day budget deficit. */
};

/*! @brief Movement mode requested by the care layer (mapped to CppMode by the caller). */
enum CareMode : uint8_t {
    CARE_MODE_IDLE   = 0,  /**< Stay put. */
    CARE_MODE_LIGHT  = 1,  /**< Light seeking. */
    CARE_MODE_SHADOW = 2   /**< Shadow seeking. */
};

/*!
    @struct CareInputs
    @brief  Snapshot of everything careStep() needs to take one decision.
    @details All fields are plain values so the function stays pure and
             host-testable; the firmware module (Care.cpp) fills them from
             Sensors/Movement/counters once per care tick.
*/
struct CareInputs {
    bool     timeValid;             /**< true if a reliable clock is available (RTC or software fallback). */
    uint8_t  hourOfDay;             /**< Current hour, 0..23. */
    uint8_t  dayStartHour;          /**< Daylight window start (inclusive). */
    uint8_t  dayEndHour;            /**< Daylight window end (exclusive). */
    float    budgetPct;             /**< Daily light budget achieved, 0..100+ (careBudgetPct). */
    int      luxAdc;                /**< Current LDR ADC (negative = invalid). */
    uint16_t heatStreakS;           /**< Seconds of continuous ADC > profile lux_high_adc. */
    uint16_t lowLightStreakS;       /**< Seconds of continuous ADC below CARE_RESEEK_FRAC of the best recent spot. */
    uint8_t  relocationsToday;      /**< Seeking sessions started today. */
    uint16_t minutesToWindowEnd;    /**< Minutes until the daylight window closes. */
    uint8_t  growLightMinutesToday; /**< UVA top-up minutes consumed today. */
    bool     atLocalMax;            /**< Seeking: ADC stopped improving (settle here). */
    bool     seekTimedOut;          /**< Seeking: session exceeded CARE_SEEK_TIMEOUT_S. */
};

/*!
    @struct CareOutputs
    @brief  Result of one careStep(): next state and desired actuation.
*/
struct CareOutputs {
    uint8_t next;          /**< Next ::CareState. */
    uint8_t mode;          /**< Desired ::CareMode for the movement layer. */
    bool    growLightOn;   /**< true if the UVA lights should be on (TOP_UP only). */
    bool    startSeekScan; /**< true on a transition into a SEEK_* state: run the light scan. */
};

/*!
    @brief    True if the profile's heat proxy or a met budget asks for shade.
    @details  The heat proxy requires a *valid* ADC above `lux_high_adc` for at
              least CARE_HEAT_STREAK_S: a blind LDR can never push the robot
              into shade seeking (fail-safe), and short glints are filtered out.
    @param[in] in Inputs snapshot (lux, heat streak, budget).
    @param[in] p  Active plant profile (`lux_high_adc`).
    @return   true if shade should be sought now.
*/
static inline bool careShadeWanted(const CareInputs& in, const PlantProfile& p) {
    const bool heat = (in.luxAdc >= 0) &&
                      (in.luxAdc > (int)p.lux_high_adc) &&
                      (in.heatStreakS >= CARE_HEAT_STREAK_S);
    return heat || (in.budgetPct >= CARE_BUDGET_DONE_PCT);
}

/*!
    @brief    True if a (re)location toward light is justified right now.
    @details  Gated by the daily relocation cap (`max_reloc_per_day`, an energy
              proxy) and by a met budget. The first seek of the day is always
              granted; later re-seeks additionally require a *sustained* light
              loss at the current spot (CARE_RESEEK_STREAK_S), so a passing
              cloud does not send the robot wandering.
    @param[in] in         Inputs snapshot (budget, relocations, low-light streak).
    @param[in] p          Active plant profile (`max_reloc_per_day`).
    @param[in] firstOfDay true when this is the morning decision out of CARE_NIGHT.
    @return   true if a light-seeking session should start now.
*/
static inline bool careSeekWanted(const CareInputs& in, const PlantProfile& p,
                                  bool firstOfDay) {
    if (in.budgetPct >= CARE_BUDGET_DONE_PCT) return false;
    if (in.relocationsToday >= p.max_reloc_per_day) return false;
    if (firstOfDay) return true;
    // Re-seek only after a sustained light loss at the current spot.
    return (in.luxAdc >= 0) && (in.lowLightStreakS >= CARE_RESEEK_STREAK_S);
}

/*!
    @brief    True if the end-of-day UVA top-up should run.
    @details  Only within the last CARE_TOPUP_START_MIN minutes of the daylight
              window, only if the budget is meaningfully short of the target
              (below CARE_TOPUP_BUDGET_PCT) and only under the daily UVA cap
              (`grow_light_max_min`): artificial light is the last resort,
              never a substitute for relocation during the day.
    @param[in] in Inputs snapshot (budget, minutes to window end, UVA minutes).
    @param[in] p  Active plant profile (`grow_light_max_min`).
    @return   true if the grow lights should compensate the deficit now.
*/
static inline bool careTopUpWanted(const CareInputs& in, const PlantProfile& p) {
    return (in.budgetPct < CARE_TOPUP_BUDGET_PCT) &&
           (in.minutesToWindowEnd <= CARE_TOPUP_START_MIN) &&
           (in.growLightMinutesToday < p.grow_light_max_min);
}

/*!
    @brief    One transition of the daily care state machine.

    @details  Pure function: given the current state, the inputs snapshot and
              the plant profile, returns the next state and the desired
              actuation. Priority order (mirrors the decision table in
              docs/Plant_Care_Design.md §6):
              1. no reliable clock or outside the daylight window → CARE_NIGHT
                 (parked, lights off — fail-safe consistent with
                 withinDaylightWindow in SensorPolicy.h);
              2. shade demand (heat proxy / budget met) preempts everything;
              3. light seeking (first of the day, or sustained light loss);
              4. end-of-day UVA top-up;
              5. otherwise: stay (BASK/SHELTER accumulate passively).

              The degraded-mode/manual-override guards are enforced by the
              caller (Care.cpp): this function only expresses the *intent*.

    @param[in] state Current ::CareState.
    @param[in] in    Inputs snapshot.
    @param[in] p     Active plant profile.

    @return   ::CareOutputs with next state, movement mode, grow-light demand
              and whether a new light scan must be started.
*/
static inline CareOutputs careStep(uint8_t state, const CareInputs& in,
                                   const PlantProfile& p) {
    CareOutputs out;
    out.next          = state;
    out.mode          = CARE_MODE_IDLE;
    out.growLightOn   = false;
    out.startSeekScan = false;

    // Rule 1: no clock, or night → park. (Also the daily-reset anchor point.)
    const bool day = in.timeValid &&
                     in.hourOfDay >= in.dayStartHour && in.hourOfDay < in.dayEndHour;
    if (!day) {
        out.next = CARE_NIGHT;
        return out;
    }

    const bool shade = careShadeWanted(in, p);

    switch (state) {
        case CARE_NIGHT: {
            // Window just opened: morning decision.
            if (shade) {
                out.next = CARE_SEEK_SHADE; out.mode = CARE_MODE_SHADOW; out.startSeekScan = true;
            } else if (careSeekWanted(in, p, /*firstOfDay=*/true)) {
                out.next = CARE_SEEK_SUN; out.mode = CARE_MODE_LIGHT; out.startSeekScan = true;
            } else {
                out.next = CARE_BASK;
            }
            break;
        }

        case CARE_SEEK_SUN: {
            if (shade) {
                out.next = CARE_SEEK_SHADE; out.mode = CARE_MODE_SHADOW; out.startSeekScan = true;
            } else if (in.atLocalMax || in.seekTimedOut) {
                out.next = CARE_BASK;                 // settle here and accumulate
            } else {
                out.mode = CARE_MODE_LIGHT;           // keep climbing the gradient
            }
            break;
        }

        case CARE_BASK: {
            if (shade) {
                out.next = CARE_SEEK_SHADE; out.mode = CARE_MODE_SHADOW; out.startSeekScan = true;
            } else if (careSeekWanted(in, p, /*firstOfDay=*/false)) {
                out.next = CARE_SEEK_SUN; out.mode = CARE_MODE_LIGHT; out.startSeekScan = true;
            } else if (careTopUpWanted(in, p)) {
                out.next = CARE_TOP_UP; out.growLightOn = true;
            }
            break;
        }

        case CARE_SEEK_SHADE: {
            const bool heatGone = (in.luxAdc >= 0) && (in.luxAdc <= (int)p.lux_high_adc) &&
                                  (in.heatStreakS == 0);
            if (in.atLocalMax || in.seekTimedOut || heatGone) {
                out.next = CARE_SHELTER;
            } else {
                out.mode = CARE_MODE_SHADOW;
            }
            break;
        }

        case CARE_SHELTER: {
            const bool heatOver = (in.heatStreakS == 0) &&
                                  (in.luxAdc >= 0) && (in.luxAdc <= (int)p.lux_high_adc);
            if (!shade && heatOver && in.budgetPct < CARE_BUDGET_DONE_PCT) {
                out.next = CARE_BASK;                 // resume the normal daylight cycle
            } else if (!shade && careTopUpWanted(in, p)) {
                out.next = CARE_TOP_UP; out.growLightOn = true;
            }
            break;
        }

        case CARE_TOP_UP: {
            if (in.budgetPct >= CARE_BUDGET_DONE_PCT ||
                in.growLightMinutesToday >= p.grow_light_max_min) {
                out.next = CARE_BASK;                 // target met or cap hit: lights off
            } else {
                out.growLightOn = true;
            }
            break;
        }

        default:
            out.next = CARE_NIGHT;                    // unknown state: fail-safe park
            break;
    }
    return out;
}

/*! @} */ // MegaPolicy

#endif // CARE_POLICY_H
