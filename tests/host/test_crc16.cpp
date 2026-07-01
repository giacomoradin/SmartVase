// =====================================================================
// Unit test NATIVO (host, g++) del CRC16-CCITT del firmware.
//
// Perche': il framing seriale Hub<->Mega e i blob EEPROM dipendono dal CRC.
// In passato Hub e Mega usavano algoritmi diversi (CCITT vs IBM) e OGNI frame
// veniva scartato: il link era morto. Questo test "pinna" l'invariante:
//   - il CRC del Mega rispetta vettori noti (CRC-16/XMODEM);
//   - l'algoritmo dell'Hub produce lo STESSO valore del Mega.
//
// Compila il codice REALE del Mega (non una copia) via lo shim Arduino.h.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>

// Logica reale del Mega (Crc16.cpp -> Crc16.h -> <Arduino.h> shim).
#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Crc16.cpp"

// Replica dell'algoritmo dell'Hub (SerialManager::crc16_update/crc16):
// CCITT poly 0x1021, init 0x0000, MSB-first. DEVE coincidere col Mega.
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

    // 1) Vettore noto CRC-16/XMODEM (poly 0x1021, init 0x0000): "123456789" -> 0x31C3
    const char* s = "123456789";
    CHECK(crc16_ccitt((const uint8_t*)s, strlen(s)) == 0x31C3,
          "CRC16-CCITT(\"123456789\") == 0x31C3 (vettore XMODEM)");

    // 2) Buffer vuoto -> init 0x0000
    CHECK(crc16_ccitt((const uint8_t*)"", 0) == 0x0000, "CRC(vuoto) == 0x0000");

    // 3) Byte 0x00 -> resta 0x0000 (init 0, nessun bit a 1)
    const uint8_t zero = 0x00;
    CHECK(crc16_ccitt(&zero, 1) == 0x0000, "CRC(0x00) == 0x0000");

    // 4) Un frame con SOF tipico produce un CRC non banale (sanity, non-zero)
    const uint8_t frame[] = {0xAA, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK(crc16_ccitt(frame, sizeof(frame)) != 0x0000, "CRC(frame) != 0 (sanity)");

    // 5) INVARIANTE DI PROTOCOLLO: Mega e Hub concordano su input vari/lunghezze.
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
    CHECK(allMatch, "CRC Mega == CRC Hub su tutti i campioni (link seriale coerente)");

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
