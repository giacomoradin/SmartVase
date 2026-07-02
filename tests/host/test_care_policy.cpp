// =====================================================================
// Native host unit test (g++) for the PURE plant-care policy (L2):
//   - carePresetProfile  (per-plant setpoints sanity)
//   - CareBudget         (daily light-budget accounting + auto-calibration)
//   - careBestScanSector (rotating light-scan sector selection)
//   - careDoseWanted     (dose/soak/verify hysteresis)
//   - careStep           (daily care state machine, decision table §6 of
//                         docs/Plant_Care_Design.md)
// Includes the REAL code (CarePolicy.h, pure header: <stdint.h>+<math.h>).
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

// Baseline daylight inputs: 10:00, window 06-20, seeking justified.
static CareInputs mkDay() {
    CareInputs in;
    in.timeValid             = true;
    in.hourOfDay             = 10;
    in.dayStartHour          = 6;
    in.dayEndHour            = 20;
    in.budgetPct             = 20.0f;
    in.luxAdc                = 400;
    in.heatStreakS           = 0;
    in.lowLightStreakS       = 0;
    in.relocationsToday      = 0;
    in.minutesToWindowEnd    = 600;
    in.growLightMinutesToday = 0;
    in.atLocalMax            = false;
    in.seekTimedOut          = false;
    return in;
}

int main() {
    printf("== test_care_policy ==\n");
    const PlantProfile shade  = carePresetProfile(CARE_PLANT_SHADE);
    const PlantProfile medium = carePresetProfile(CARE_PLANT_MEDIUM);
    const PlantProfile sun    = carePresetProfile(CARE_PLANT_SUN);

    // --- Presets sanity ---
    CHECK(shade.light_target_min < medium.light_target_min &&
          medium.light_target_min < sun.light_target_min,
          "light target increases shade < medium < sun");
    CHECK(shade.soil_wet_adc > shade.soil_dry_adc &&
          medium.soil_wet_adc > medium.soil_dry_adc &&
          sun.soil_wet_adc > sun.soil_dry_adc,
          "soil hysteresis band wet > dry in every preset");
    CHECK(sun.lux_high_adc > 1023, "sun preset never shades for heat (threshold above ADC max)");
    CHECK(medium.max_doses_per_day > 0 && medium.max_reloc_per_day > 0 &&
          medium.dose_ms > 0 && medium.soak_min > 0,
          "medium preset caps and dose are non-zero");
    {
        const PlantProfile unk = carePresetProfile(99);
        CHECK(unk.light_target_min == medium.light_target_min,
              "unknown kind falls back to the MEDIUM preset");
    }

    // --- CareBudget: accumulation and auto-calibration ---
    {
        CareBudget b = careBudgetInit();
        CHECK(b.accum_s == 0.0f && b.day_max_adc == CARE_LUX_FLOOR_ADC,
              "init: empty accumulator, floor reference");

        careBudgetUpdate(b, -1, 60.0f);
        CHECK(b.accum_s == 0.0f, "invalid lux (-1) does not accumulate");
        careBudgetUpdate(b, 500, 0.0f);
        CHECK(b.accum_s == 0.0f, "dt=0 does not accumulate");

        // 60 s at the reference max = 60 full-light seconds = 100% of a 1-min target.
        careBudgetUpdate(b, (int)CARE_LUX_FLOOR_ADC, 60.0f);
        CHECK(fabsf(careBudgetPct(b, 1) - 100.0f) < 0.5f,
              "60 s at reference max -> 100% of a 1-minute target");

        // A brighter sample raises the reference: the same ADC now counts as full light.
        careBudgetUpdate(b, 800, 10.0f);
        CHECK(fabsf(b.day_max_adc - 800.0f) < 0.01f, "day max rises instantly to a new peak");

        // Dark-room sample barely contributes once the reference is high.
        const float before = b.accum_s;
        careBudgetUpdate(b, 11, 60.0f);
        CHECK(b.accum_s - before < 2.0f, "ADC 11 against ref 800 contributes < 2 s in a minute");

        careBudgetDailyReset(b);
        CHECK(b.accum_s == 0.0f, "daily reset clears the accumulator");
        CHECK(fabsf(b.day_max_adc - 800.0f * CARE_DAYMAX_DECAY) < 0.01f,
              "daily reset decays the reference max");
        for (int i = 0; i < 20; ++i) careBudgetDailyReset(b);
        CHECK(b.day_max_adc >= CARE_LUX_FLOOR_ADC, "reference max never decays below the floor");

        CHECK(careBudgetPct(b, 0) == 100.0f, "zero target -> always 100% (nothing to chase)");
    }

    // --- careBestScanSector ---
    {
        float m1[6] = {100, 300, 250, NAN, 50, 120};
        CHECK(careBestScanSector(m1, 6, true)  == 1, "light scan picks the brightest sector");
        CHECK(careBestScanSector(m1, 6, false) == 4, "shade scan picks the darkest sector");
        float m2[3] = {NAN, NAN, NAN};
        CHECK(careBestScanSector(m2, 3, true) == -1, "all-NaN scan -> -1 (no data)");
        float m3[4] = {-5.0f, NAN, 42.0f, -1.0f};
        CHECK(careBestScanSector(m3, 4, false) == 2, "negative means are skipped like NaN");
    }

    // --- careDoseWanted (LOWER ADC = drier, firmware convention) ---
    {
        const uint16_t dry = medium.soil_dry_adc, wet = medium.soil_wet_adc;
        CHECK(!careDoseWanted(-1, dry, wet, false), "invalid soil reading never starts a dose");
        CHECK(!careDoseWanted(-1, dry, wet, true),  "invalid soil reading never continues a cycle");
        CHECK(!careDoseWanted((int)dry + 10, dry, wet, false), "soil above dry threshold -> no cycle start");
        CHECK(careDoseWanted((int)dry - 10, dry, wet, false),  "soil below dry threshold -> cycle starts");
        CHECK(careDoseWanted((int)dry + 10, dry, wet, true),   "active cycle keeps dosing between dry and wet");
        CHECK(!careDoseWanted((int)wet, dry, wet, true),       "active cycle stops at the wet target");
    }

    // --- careStep: night / clock fail-safe ---
    {
        CareInputs in = mkDay();
        in.timeValid = false;
        CareOutputs o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_NIGHT && o.mode == CARE_MODE_IDLE && !o.growLightOn,
              "no reliable clock -> parked in NIGHT, lights off");

        in = mkDay();
        in.hourOfDay = 22;
        o = careStep(CARE_TOP_UP, in, medium);
        CHECK(o.next == CARE_NIGHT && !o.growLightOn,
              "window closed -> NIGHT even from TOP_UP (lights off)");
    }

    // --- careStep: morning decision from NIGHT ---
    {
        CareInputs in = mkDay();
        CareOutputs o = careStep(CARE_NIGHT, in, medium);
        CHECK(o.next == CARE_SEEK_SUN && o.mode == CARE_MODE_LIGHT && o.startSeekScan,
              "morning + budget deficit -> SEEK_SUN with a fresh light scan");

        in = mkDay();
        in.budgetPct = 100.0f;
        o = careStep(CARE_NIGHT, in, medium);
        CHECK(o.next == CARE_SEEK_SHADE && o.mode == CARE_MODE_SHADOW && o.startSeekScan,
              "morning with budget already met -> seek shade");

        in = mkDay();
        in.relocationsToday = medium.max_reloc_per_day;
        o = careStep(CARE_NIGHT, in, medium);
        CHECK(o.next == CARE_BASK, "morning with relocation cap exhausted -> just bask");
    }

    // --- careStep: SEEK_SUN progress and settling ---
    {
        CareInputs in = mkDay();
        CareOutputs o = careStep(CARE_SEEK_SUN, in, medium);
        CHECK(o.next == CARE_SEEK_SUN && o.mode == CARE_MODE_LIGHT && !o.startSeekScan,
              "seeking in progress -> keep climbing, no new scan");

        in.atLocalMax = true;
        o = careStep(CARE_SEEK_SUN, in, medium);
        CHECK(o.next == CARE_BASK && o.mode == CARE_MODE_IDLE,
              "local maximum reached -> settle and BASK");

        in = mkDay();
        in.seekTimedOut = true;
        o = careStep(CARE_SEEK_SUN, in, medium);
        CHECK(o.next == CARE_BASK, "seek timeout -> settle and BASK (no endless wandering)");
    }

    // --- careStep: BASK triggers (shade / re-seek / anti-nomadism / top-up) ---
    {
        CareInputs in = mkDay();
        CareOutputs o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_BASK, "good light, budget in progress -> keep basking");

        in.budgetPct = 100.0f;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_SEEK_SHADE && o.startSeekScan,
              "budget met -> seek shade (rest position)");

        in = mkDay();
        in.luxAdc = (int)medium.lux_high_adc + 50;
        in.heatStreakS = CARE_HEAT_STREAK_S;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_SEEK_SHADE, "sustained over-light (heat proxy) -> seek shade");

        in = mkDay();
        in.heatStreakS = CARE_HEAT_STREAK_S; // streak alone, ADC back below threshold
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_BASK, "heat streak without current over-light -> no shade");

        in = mkDay();
        in.lowLightStreakS = CARE_RESEEK_STREAK_S;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_SEEK_SUN && o.startSeekScan,
              "sustained light loss at the spot -> relocate toward light");

        in.relocationsToday = medium.max_reloc_per_day;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_BASK, "relocation cap exhausted -> stay (anti-nomadism)");

        in = mkDay();
        in.budgetPct          = 50.0f;
        in.minutesToWindowEnd = 30;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_TOP_UP && o.growLightOn,
              "end of day with budget deficit -> UVA top-up");

        in.budgetPct = CARE_TOPUP_BUDGET_PCT + 5.0f;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_BASK && !o.growLightOn,
              "budget close enough to target -> no top-up");

        in.budgetPct = 50.0f;
        in.growLightMinutesToday = medium.grow_light_max_min;
        o = careStep(CARE_BASK, in, medium);
        CHECK(o.next == CARE_BASK && !o.growLightOn,
              "daily UVA cap consumed -> no top-up");
    }

    // --- careStep: SEEK_SHADE / SHELTER cycle ---
    {
        CareInputs in = mkDay();
        in.budgetPct = 100.0f;                 // shading because the budget is met
        in.luxAdc    = (int)medium.lux_high_adc + 100;
        in.heatStreakS = CARE_HEAT_STREAK_S;
        CareOutputs o = careStep(CARE_SEEK_SHADE, in, medium);
        CHECK(o.next == CARE_SEEK_SHADE && o.mode == CARE_MODE_SHADOW,
              "still in bright light -> keep seeking shade");

        in.luxAdc = 100;                       // found a dark spot
        in.heatStreakS = 0;
        o = careStep(CARE_SEEK_SHADE, in, medium);
        CHECK(o.next == CARE_SHELTER, "dark spot found -> SHELTER");

        // Shelter holds while the budget is met...
        o = careStep(CARE_SHELTER, in, medium);
        CHECK(o.next == CARE_SHELTER, "budget met -> rest in shelter");

        // ...and resumes the cycle if the budget drops back (new day handled by reset).
        in.budgetPct = 40.0f;
        o = careStep(CARE_SHELTER, in, medium);
        CHECK(o.next == CARE_BASK, "budget deficit and no heat -> resume from BASK");
    }

    // --- careStep: TOP_UP termination ---
    {
        CareInputs in = mkDay();
        in.budgetPct          = 70.0f;
        in.minutesToWindowEnd = 20;
        CareOutputs o = careStep(CARE_TOP_UP, in, medium);
        CHECK(o.next == CARE_TOP_UP && o.growLightOn, "deficit persists -> lights stay on");

        in.budgetPct = 100.0f;
        o = careStep(CARE_TOP_UP, in, medium);
        CHECK(o.next == CARE_BASK && !o.growLightOn, "target reached -> lights off");

        in.budgetPct = 70.0f;
        in.growLightMinutesToday = medium.grow_light_max_min;
        o = careStep(CARE_TOP_UP, in, medium);
        CHECK(o.next == CARE_BASK && !o.growLightOn, "daily cap hit -> lights off");
    }

    // --- careStep: unknown state fail-safe ---
    {
        CareInputs in = mkDay();
        CareOutputs o = careStep(200, in, medium);
        CHECK(o.next == CARE_NIGHT, "unknown state -> fail-safe park");
    }

    if (g_failures == 0) {
        printf("RESULT: ALL PASSED\n");
        return 0;
    }
    printf("RESULT: %d FAILURE(S)\n", g_failures);
    return 1;
}
