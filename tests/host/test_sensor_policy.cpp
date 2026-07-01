// =====================================================================
// Unit test NATIVO (host, g++) delle policy PURE derivate dai sensori:
//   - tankConsideredEmpty (fail-safe pompa su US4)
//   - seekWantsTurn       (decisione light/shadow seeking)
// Include il codice REALE (SensorPolicy.h, header puro: <stdint.h>+<math.h>).
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
    CHECK(tankConsideredEmpty(NAN, 20),     "US4 NaN -> vuota (fail-safe)");
    CHECK(tankConsideredEmpty(25.0f, 20),   "25cm > soglia 20 -> vuota");
    CHECK(!tankConsideredEmpty(15.0f, 20),  "15cm < soglia 20 -> NON vuota");
    CHECK(!tankConsideredEmpty(20.0f, 20),  "20cm == soglia -> NON vuota (solo > e' vuota)");

    // --- seekWantsTurn(seekingLight, seekingShadow, lux, threshold=600) ---
    CHECK(!seekWantsTurn(false, false, 500, 600), "IDLE -> no turn");
    CHECK(!seekWantsTurn(true,  false, -1,  600), "lux invalido (<0) -> no turn");
    CHECK(seekWantsTurn(true,  false, 500, 600),  "LIGHT + buio (500<600) -> turn");
    CHECK(!seekWantsTurn(true,  false, 700, 600), "LIGHT + luminoso -> no turn");
    CHECK(seekWantsTurn(false, true,  700, 600),  "SHADOW + luminoso (700>600) -> turn");
    CHECK(!seekWantsTurn(false, true,  500, 600), "SHADOW + buio -> no turn");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
