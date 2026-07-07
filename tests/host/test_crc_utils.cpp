// =====================================================================
// NATIVE unit test (host, g++) of the Hub's shared CRCs (crc_utils).
// Pins both polynomials with known standard vectors:
//   - crc16_ccitt : CRC-16/XMODEM("123456789") = 0x31C3 (serial link)
//   - crc16_ibm   : CRC-16/ARC   ("123456789") = 0xBB3D (NVS)
// Includes the REAL code (crc_utils.cpp, pure header stdint/stddef).
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/crc_utils.cpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

int main() {
    printf("== test_crc_utils ==\n");
    const uint8_t* s = (const uint8_t*)"123456789";

    CHECK(crc16_ccitt(s, 9) == 0x31C3, "crc16_ccitt(\"123456789\") == 0x31C3 (XMODEM)");
    CHECK(crc16_ibm(s, 9)   == 0xBB3D, "crc16_ibm(\"123456789\")   == 0xBB3D (ARC)");
    CHECK(crc16_ccitt((const uint8_t*)"", 0) == 0x0000, "crc16_ccitt(empty) == 0");
    CHECK(crc16_ibm((const uint8_t*)"", 0)   == 0x0000, "crc16_ibm(empty) == 0");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
