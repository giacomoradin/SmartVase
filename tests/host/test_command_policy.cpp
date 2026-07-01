// =====================================================================
// Unit test NATIVO (host, g++) delle policy di comando PURE del Mega.
// Verifica la logica usata in Communication::executeCommand:
//   - waterAllowed       (anti over-watering / flood)
//   - motionParamsChanged(anti-usura EEPROM)
// Include il codice REALE (CommandPolicy.h, header puro: solo <stdint.h>).
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
    CHECK(waterAllowed(0, 0, 5000),        "prima irrigazione (last=0) sempre consentita");
    CHECK(!waterAllowed(1000, 1, 5000),    "delta 999ms -> rifiutata");
    CHECK(!waterAllowed(5999, 1000, 5000), "delta 4999ms -> rifiutata");
    CHECK(waterAllowed(6000, 1000, 5000),  "delta 5000ms esatti -> consentita");
    CHECK(waterAllowed(20000, 1000, 5000), "delta ampio -> consentita");

    // --- motionParamsChanged(curRev,curTurn,newRev,newTurn) ---
    CHECK(!motionParamsChanged(1000, 1200, 1000, 1200), "identici -> nessuna scrittura EEPROM");
    CHECK(motionParamsChanged(1000, 1200, 1000, 1300),  "turn diverso -> scrittura");
    CHECK(motionParamsChanged(1000, 1200, 900, 1200),   "reverse diverso -> scrittura");
    CHECK(motionParamsChanged(0, 0, 1, 1),              "da zero a valori -> scrittura");

    // --- clampWaterDurationMs(ms, max=30000) ---
    CHECK(clampWaterDurationMs(5000, 30000)  == 5000,  "water 5s invariata");
    CHECK(clampWaterDurationMs(60000, 30000) == 30000, "water 60s -> clamp 30s");
    CHECK(clampWaterDurationMs(0, 30000)     == 0,     "water 0 invariata");

    // --- clampMotionParamMs(ms, lo=100, hi=5000) ---
    CHECK(clampMotionParamMs(1200, 100, 5000)  == 1200, "motion 1200 invariato");
    CHECK(clampMotionParamMs(50, 100, 5000)    == 100,  "motion 50 -> min 100");
    CHECK(clampMotionParamMs(99999, 100, 5000) == 5000, "motion 99999 -> max 5000");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
