// =====================================================================
// NATIVE unit test (host, g++) of the firmware's CRC16-CCITT.
//
// Why: the Hub<->Mega serial framing and EEPROM blobs depend on the CRC.
// In the past, Hub and Mega used different algorithms (CCITT vs IBM) and EVERY frame
// was discarded: the link was dead. This test "pins" the invariant:
//   - the Mega's CRC respects known vectors (CRC-16/XMODEM);
//   - the Hub's algorithm produces the SAME value as the Mega.
//
// Compiles the REAL Mega code (not a copy) via the Arduino.h shim.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>

// Real Mega logic (Crc16.cpp -> Crc16.h -> <Arduino.h> shim).
#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Crc16.cpp"

// Replica of the Hub's algorithm (SerialManager::crc16_update/crc16):
// CCITT poly 0x1021, init 0x0000, MSB-first. MUST match the Mega.
static uint16_t hub_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

int main() {
    printf("== test_crc16 ==\n");

    // 1) Known CRC-16/XMODEM vector (poly 0x1021, init 0x0000): "123456789" -> 0x31C3
    const char* s = "123456789";
    CHECK(crc16_ccitt((const uint8_t*)s, strlen(s)) == 0x31C3,
          "CRC16-CCITT(\"123456789\") == 0x31C3 (XMODEM vector)");

    // 2) Empty buffer -> init 0x0000
    CHECK(crc16_ccitt((const uint8_t*)"", 0) == 0x0000, "CRC(empty) == 0x0000");

    // 3) Byte 0x00 -> remains 0x0000 (init 0, no bit at 1)
    const uint8_t zero = 0x00;
    CHECK(crc16_ccitt(&zero, 1) == 0x0000, "CRC(0x00) == 0x0000");

    // 4) A frame with a typical SOF produces a non-trivial CRC (sanity, non-zero)
    const uint8_t frame[] = {0xAA, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK(crc16_ccitt(frame, sizeof(frame)) != 0x0000, "CRC(frame) != 0 (sanity)");

    // 5) PROTOCOL INVARIANT: Mega and Hub agree on various inputs/lengths.
    const uint8_t samples[][8] = {
        {0xAA, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
        {0x00, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60},
        {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78},
    };
    bool allMatch = true;
    for (const auto& smp : samples) {
        for (size_t len = 0; len <= 8; ++len) {
            if (crc16_ccitt(smp, len) != hub_crc16(smp, len)) allMatch = false;
        }
    }
    CHECK(allMatch, "Mega CRC == Hub CRC on all samples (coherent serial link)");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
