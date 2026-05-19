#include "Crc16.h"

uint16_t crc16_ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0000;
    while (length--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}
