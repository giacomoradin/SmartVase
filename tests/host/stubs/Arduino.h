// Shim minimale di <Arduino.h> per compilare i moduli "puri" del firmware
// come unit test NATIVI con g++ (offline, senza la piattaforma native di
// PlatformIO, che non e' in cache su questa macchina).
//
// Espone solo i tipi di base che servono ai moduli logici sotto test
// (es. Crc16). NON e' un'emulazione di Arduino: i moduli che dipendono da
// HW reale (Serial, analogRead, millis...) non vanno testati per questa via
// ma rifattorizzati per isolare la logica pura.
#ifndef SMARTVASE_HOST_ARDUINO_SHIM_H
#define SMARTVASE_HOST_ARDUINO_SHIM_H

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>

unsigned long millis();

#endif // SMARTVASE_HOST_ARDUINO_SHIM_H
