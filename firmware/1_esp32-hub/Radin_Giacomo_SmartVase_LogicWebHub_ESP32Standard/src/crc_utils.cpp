#include "crc_utils.h"

// CCITT poly 0x1021, MSB-first (identico al Mega).
uint16_t crc16_ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

// IBM/ARC poly 0xA001, LSB-first (uso NVS interno Hub).
uint16_t crc16_ibm(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc =  crc >> 1;
        }
    }
    return crc;
}
