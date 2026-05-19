#ifndef CRC16_H
#define CRC16_H

#include <Arduino.h>

// CRC-CCITT (poly 0x1021, init 0x0000, no reflection, no xor-out).
// Usato sia dal framing seriale Hub<->Mega che dai blob EEPROM.
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

#endif // CRC16_H
