// =====================================================================
// NATIVE unit test (host, g++) of the PURE sensor-derived policies:
//   - tankConsideredEmpty (pump fail-safe on US4)
//   - seekWantsTurn       (light/shadow seeking decision)
// Includes the REAL code (SensorPolicy.h, pure header: <stdint.h>+<math.h>).
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/SensorPolicy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

int main() {
    printf("== test_sensor_policy ==\n");

    // --- tankConsideredEmpty(waterLevelCm, thresholdCm=20) ---
    CHECK(tankConsideredEmpty(NAN, 20),     "US4 NaN -> empty (fail-safe)");
    CHECK(tankConsideredEmpty(25.0f, 20),   "25cm > threshold 20 -> empty");
    CHECK(!tankConsideredEmpty(15.0f, 20),  "15cm < threshold 20 -> NOT empty");
    CHECK(!tankConsideredEmpty(20.0f, 20),  "20cm == threshold -> NOT empty (only > is empty)");

    // --- seekWantsTurn(seekingLight, seekingShadow, lux, threshold=600) ---
    CHECK(!seekWantsTurn(false, false, 500, 600), "IDLE -> no turn");
    CHECK(!seekWantsTurn(true,  false, -1,  600), "invalid lux (<0) -> no turn");
    CHECK(seekWantsTurn(true,  false, 500, 600),  "LIGHT + dark (500<600) -> turn");
    CHECK(!seekWantsTurn(true,  false, 700, 600), "LIGHT + bright -> no turn");
    CHECK(seekWantsTurn(false, true,  700, 600),  "SHADOW + bright (700>600) -> turn");
    CHECK(!seekWantsTurn(false, true,  500, 600), "SHADOW + dark -> no turn");

    // --- medianOf3(a,b,c) — anti-bounce sonar pre-filter (NaN-aware) ---
    CHECK(medianOf3(10.0f, 200.0f, 11.0f) == 11.0f, "spike 200 in {10,200,11} -> median 11 (rejected)");
    CHECK(medianOf3(30.0f, 31.0f, 32.0f) == 31.0f,  "sorted {30,31,32} -> median 31");
    CHECK(medianOf3(50.0f, NAN, 60.0f) == 55.0f,    "one NaN -> average of the 2 valid (55)");
    CHECK(medianOf3(42.0f, NAN, NAN) == 42.0f,      "two NaN -> the single valid value (42)");
    CHECK(std::isnan(medianOf3(NAN, NAN, NAN)),      "all NaN -> NaN");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
