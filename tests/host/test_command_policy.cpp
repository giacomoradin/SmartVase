// =====================================================================
// NATIVE unit test (host, g++) of the Mega's PURE command policies.
// Verifies the logic used in Communication::executeCommand:
//   - waterAllowed       (anti over-watering / flood)
//   - motionParamsChanged(EEPROM wear-leveling)
// Includes the REAL code (CommandPolicy.h, pure header: only <stdint.h>).
// =====================================================================
#include <cstdio>
#include <cstdint>

#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/CommandPolicy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

int main() {
    printf("== test_command_policy ==\n");

    // --- waterAllowed(now, lastAccepted, minInterval=5000) ---
    CHECK(waterAllowed(0, 0, 5000),        "first watering (last=0) always allowed");
    CHECK(!waterAllowed(1000, 1, 5000),    "delta 999ms -> rejected");
    CHECK(!waterAllowed(5999, 1000, 5000), "delta 4999ms -> rejected");
    CHECK(waterAllowed(6000, 1000, 5000),  "exact delta 5000ms -> allowed");
    CHECK(waterAllowed(20000, 1000, 5000), "large delta -> allowed");

    // --- motionParamsChanged(curRev,curTurn,newRev,newTurn) ---
    CHECK(!motionParamsChanged(1000, 1200, 1000, 1200), "identical -> no EEPROM write");
    CHECK(motionParamsChanged(1000, 1200, 1000, 1300),  "different turn -> write");
    CHECK(motionParamsChanged(1000, 1200, 900, 1200),   "different reverse -> write");
    CHECK(motionParamsChanged(0, 0, 1, 1),              "from zero to values -> write");

    // --- clampWaterDurationMs(ms, max=30000) ---
    CHECK(clampWaterDurationMs(5000, 30000)  == 5000,  "water 5s unchanged");
    CHECK(clampWaterDurationMs(60000, 30000) == 30000, "water 60s -> clamp 30s");
    CHECK(clampWaterDurationMs(0, 30000)     == 0,     "water 0 unchanged");

    // --- clampMotionParamMs(ms, lo=100, hi=5000) ---
    CHECK(clampMotionParamMs(1200, 100, 5000)  == 1200, "motion 1200 unchanged");
    CHECK(clampMotionParamMs(50, 100, 5000)    == 100,  "motion 50 -> min 100");
    CHECK(clampMotionParamMs(99999, 100, 5000) == 5000, "motion 99999 -> max 5000");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
