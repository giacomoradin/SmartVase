#ifndef CRC_UTILS_H
#define CRC_UTILS_H

#include <stdint.h>
#include <stddef.h>

// Utility CRC16 condivise dell'Hub (prima duplicate in SerialManager e
// ConfigManager). Due polinomi DIVERSI di proposito, per due usi diversi:

// CRC16-CCITT (poly 0x1021, init 0x0000, MSB-first, no reflect/xor-out):
// framing seriale Hub<->Mega. DEVE restare identico a crc16_ccitt del Mega
// (Crc16.cpp), altrimenti ogni frame viene scartato.
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

// CRC16-IBM/ARC (poly 0xA001, init 0x0000, LSB-first): integrita' dei blob di
// configurazione in NVS (uso interno dell'Hub, non scambiato con nessuno).
uint16_t crc16_ibm(const uint8_t* data, size_t length);

#endif // CRC_UTILS_H
