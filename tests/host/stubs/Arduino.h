// Minimal <Arduino.h> shim to compile the "pure" firmware modules
// as NATIVE unit tests with g++ (offline, without PlatformIO's native
// platform, which is not cached on this machine).
//
// It only exposes the base types needed by the logic modules under test
// (e.g. Crc16). It is NOT an Arduino emulation: modules that depend on
// real HW (Serial, analogRead, millis...) should not be tested this way
// but refactored to isolate pure logic.
#ifndef SMARTVASE_HOST_ARDUINO_SHIM_H
#define SMARTVASE_HOST_ARDUINO_SHIM_H

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>

unsigned long millis();

#endif // SMARTVASE_HOST_ARDUINO_SHIM_H
