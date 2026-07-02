// =====================================================================
// Native host SIMULATION test (g++) for the autonomous care layer.
//
// While test_care_policy.cpp verifies each pure rule in isolation, this
// test drives the REAL pure layer (CarePolicy.h) through whole simulated
// days at 1-minute ticks, replicating the bookkeeping of Care.cpp
// (budget accounting, streaks, counters, dose/soak cycle) in a small
// harness. It verifies the EMERGENT behavior — "the robot's day" of
// docs/Plant_Care_Design.md §6 — not just the single transitions:
//
//   A) sunny day  : NIGHT -> SEEK_SUN -> BASK (budget fills) ->
//                   SEEK_SHADE -> SHELTER -> NIGHT; no UVA needed;
//                   exactly 2 relocations.
//   B) dim day    : seek gives up, basks all day, budget stays in
//                   deficit -> UVA TOP_UP in the last hour -> NIGHT
//                   with lights off; no relocation spam.
//   C) watering   : dose/soak/verify cycle converges to the wet target
//                   in bounded doses; a stuck-dry probe hits the daily
//                   cap instead of flooding the plant.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/CarePolicy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

// ---------------------------------------------------------------------
// Minute-tick harness replicating Care.cpp's bookkeeping around the
// pure careStep()/careBudgetUpdate() calls.
// ---------------------------------------------------------------------
struct DaySim {
    // Configuration
    PlantProfile p;
    uint8_t dayStart = 6, dayEnd = 20;

    // Care state
    uint8_t    state = CARE_NIGHT;
    CareBudget budget = careBudgetInit();

    // Bookkeeping (mirrors Care.cpp)
    uint8_t  relocations = 0;
    float    growLightMin = 0.0f;
    uint16_t heatStreakS = 0;
    uint16_t lowStreakS  = 0;
    int      bestSpotAdc = -1;
    uint16_t minutesInSeek = 0;
    bool     lightOnNow = false;

    // Trace flags for the assertions
    bool sawSeekSun = false, sawBask = false, sawSeekShade = false;
    bool sawShelter = false, sawTopUp = false;

    // One 1-minute tick. `lux` is the simulated LDR ADC; `atLocalMax` /
    // `seekTimedOut` are decided by the scenario script (they come from
    // Movement supervision in the real firmware).
    void tick(uint16_t minuteOfDay, int lux, bool atLocalMax, bool seekTimedOut) {
        const uint8_t h = (uint8_t)(minuteOfDay / 60);
        const uint8_t m = (uint8_t)(minuteOfDay % 60);
        const bool day = (h >= dayStart && h < dayEnd);

        if (day) careBudgetUpdate(budget, lux, 60.0f);

        // Streaks (as in Care::tick).
        if (lux >= 0 && lux > (int)p.lux_high_adc) heatStreakS += 60;
        else                                       heatStreakS = 0;
        if (state == CARE_BASK) {
            if (lux > bestSpotAdc) bestSpotAdc = lux;
            if (bestSpotAdc > 0 && lux >= 0 &&
                (float)lux < CARE_RESEEK_FRAC * (float)bestSpotAdc) lowStreakS += 60;
            else                                                    lowStreakS = 0;
        } else {
            lowStreakS = 0;
        }
        if (state == CARE_SEEK_SUN || state == CARE_SEEK_SHADE) minutesInSeek++;
        else                                                    minutesInSeek = 0;

        CareInputs in;
        in.timeValid             = true;
        in.hourOfDay             = h;
        in.dayStartHour          = dayStart;
        in.dayEndHour            = dayEnd;
        in.budgetPct             = careBudgetPct(budget, p.light_target_min);
        in.luxAdc                = lux;
        in.heatStreakS           = heatStreakS;
        in.lowLightStreakS       = lowStreakS;
        in.relocationsToday      = relocations;
        in.minutesToWindowEnd    = day ? (uint16_t)(dayEnd * 60 - (h * 60 + m)) : 0;
        in.growLightMinutesToday = (growLightMin > 255.0f) ? 255 : (uint8_t)growLightMin;
        in.atLocalMax            = atLocalMax;
        in.seekTimedOut          = seekTimedOut;

        const CareOutputs o = careStep(state, in, p);

        if (o.next != state) {                       // as in Care::applyOutputs
            if (o.next == CARE_SEEK_SUN || o.next == CARE_SEEK_SHADE) relocations++;
            if (o.next == CARE_BASK) { bestSpotAdc = (lux >= 0) ? lux : -1; lowStreakS = 0; }
            state = o.next;
        }
        lightOnNow = o.growLightOn;
        if (o.growLightOn) growLightMin += 1.0f;

        switch (state) {
            case CARE_SEEK_SUN:   sawSeekSun   = true; break;
            case CARE_BASK:       sawBask      = true; break;
            case CARE_SEEK_SHADE: sawSeekShade = true; break;
            case CARE_SHELTER:    sawShelter   = true; break;
            case CARE_TOP_UP:     sawTopUp     = true; break;
            default: break;
        }
    }
};

int main() {
    printf("== test_care_day_sim ==\n");

    // ================================================================
    // Scenario A — sunny day, medium plant.
    // 05:00 dark; 06:00 the window opens with the robot in a dim spot
    // (ADC 100); the seek "drives" it into brighter light (ramp to 650
    // in 10 min, then local max); it basks until the budget fills, then
    // retreats to shade and shelters until night.
    // ================================================================
    {
        printf("--- scenario A: sunny day ---\n");
        DaySim s;
        s.p = carePresetProfile(CARE_PLANT_MEDIUM);   // target 240 full-light min

        uint16_t baskStartMin = 0, shadeStartMin = 0;
        for (uint16_t t = 5 * 60; t < 21 * 60; ++t) {
            int lux;
            bool atMax = false;
            if (s.state == CARE_NIGHT)         lux = 15;                    // pre-dawn / dim start spot
            else if (s.state == CARE_SEEK_SUN) {
                // Gradient climb: +55 ADC per minute of seeking, capped at 650.
                int climbed = 100 + 55 * s.minutesInSeek;
                lux = (climbed > 650) ? 650 : climbed;
                atMax = (lux >= 650);                                       // stalled at the max
            }
            else if (s.state == CARE_SEEK_SHADE) lux = 30;                  // found a dark corner
            else if (s.state == CARE_SHELTER)    lux = 30;
            else                                 lux = 650;                 // basking spot
            s.tick(t, lux, atMax, false);
            if (s.state == CARE_BASK && baskStartMin == 0)        baskStartMin  = t;
            if (s.state == CARE_SEEK_SHADE && shadeStartMin == 0) shadeStartMin = t;
        }

        CHECK(s.sawSeekSun && s.sawBask && s.sawSeekShade && s.sawShelter,
              "full day arc: SEEK_SUN -> BASK -> SEEK_SHADE -> SHELTER all visited");
        CHECK(baskStartMin >= 6 * 60 && baskStartMin <= 6 * 60 + 30,
              "settles into BASK within 30 min of the window opening");
        const float pct = careBudgetPct(s.budget, s.p.light_target_min);
        CHECK(pct >= 100.0f && pct <= 130.0f,
              "daily light budget lands at 100-130% of the target");
        CHECK(shadeStartMin > baskStartMin &&
              (shadeStartMin - 6 * 60) >= s.p.light_target_min,
              "shade retreat happens only after ~target minutes of basking");
        CHECK(s.relocations == 2,
              "exactly 2 relocations (morning sun + budget-met shade)");
        CHECK(s.growLightMin == 0.0f, "sunny day -> UVA lights never used");
        CHECK(s.state == CARE_NIGHT, "window closed -> back to NIGHT");
        CHECK(!s.lightOnNow, "NIGHT -> lights off");
    }

    // ================================================================
    // Scenario B — dim day (ADC 40 everywhere), medium plant.
    // The morning seek finds nothing better and times out; the robot
    // basks in deficit all day; the UVA top-up covers the last hour.
    // ================================================================
    {
        printf("--- scenario B: dim day ---\n");
        DaySim s;
        s.p = carePresetProfile(CARE_PLANT_MEDIUM);

        uint16_t topUpStartMin = 0;
        for (uint16_t t = 5 * 60; t < 21 * 60; ++t) {
            const bool timedOut = (s.state == CARE_SEEK_SUN) &&
                                  (s.minutesInSeek >= 3);   // seek gives up after 3 min
            s.tick(t, 40, false, timedOut);
            if (s.state == CARE_TOP_UP && topUpStartMin == 0) topUpStartMin = t;
        }

        CHECK(s.sawSeekSun && s.sawBask, "dim day: one morning attempt, then basking");
        CHECK(s.relocations == 1,
              "constant dim light -> no relocation spam (1 total)");
        const float pct = careBudgetPct(s.budget, s.p.light_target_min);
        CHECK(pct < CARE_TOPUP_BUDGET_PCT,
              "dark room -> budget stays in deficit (auto-calibration floor)");
        CHECK(s.sawTopUp && topUpStartMin >= 19 * 60,
              "UVA top-up starts within the last hour of the window");
        CHECK(s.growLightMin >= 55.0f && s.growLightMin <= 61.0f,
              "top-up runs ~60 min (bounded by the window close)");
        CHECK(s.growLightMin <= (float)s.p.grow_light_max_min,
              "top-up respects the daily UVA cap");
        CHECK(s.state == CARE_NIGHT && !s.lightOnNow,
              "window closed -> NIGHT with the lights off");
    }

    // ================================================================
    // Scenario C — dose/soak/verify watering cycle (medium plant:
    // dry 450, wet 570, soak 30 min, max 6 doses/day).
    // Replicates the ordering of Care::handleWatering.
    // ================================================================
    {
        printf("--- scenario C: watering cycle ---\n");
        const PlantProfile p = carePresetProfile(CARE_PLANT_MEDIUM);

        // C1: healthy soil response (+60 ADC per absorbed dose).
        {
            int soil = 300, doses = 0, soakLeft = 0;
            bool cycle = false, cycleDone = false;
            for (int minute = 0; minute < 12 * 60; ++minute) {
                if (soakLeft > 0) { soakLeft--; continue; }
                if (cycle && soil >= (int)p.soil_wet_adc) { cycle = false; cycleDone = true; continue; }
                if (!careDoseWanted(soil, p.soil_dry_adc, p.soil_wet_adc, cycle)) continue;
                if (doses >= p.max_doses_per_day) { cycle = false; break; }
                cycle = true; doses++;
                soil += 60;                        // absorption seen at the next check
                soakLeft = p.soak_min;
            }
            CHECK(doses == 5, "soil 300 -> wet target in exactly 5 doses (no flooding)");
            CHECK(cycleDone && soil >= (int)p.soil_wet_adc,
                  "cycle closes only once the wet target is verified");
        }

        // C2: stuck-dry probe (reads 100 forever): the daily cap must
        // stop the pump instead of watering blind all day.
        {
            int doses = 0, soakLeft = 0;
            bool cycle = false;
            for (int minute = 0; minute < 14 * 60; ++minute) {
                if (soakLeft > 0) { soakLeft--; continue; }
                if (!careDoseWanted(100, p.soil_dry_adc, p.soil_wet_adc, cycle)) continue;
                if (doses >= p.max_doses_per_day) { cycle = false; break; }
                cycle = true; doses++;
                soakLeft = p.soak_min;
            }
            CHECK(doses == p.max_doses_per_day,
                  "stuck-dry probe -> stops at the daily dose cap (fail-safe)");
        }
    }

    if (g_failures == 0) {
        printf("RESULT: ALL PASSED\n");
        return 0;
    }
    printf("RESULT: %d FAILURE(S)\n", g_failures);
    return 1;
}
