#ifndef MOCK_EEPROM_H
#define MOCK_EEPROM_H

#include <cstring>
#include <cstdint>

class EEPROMClass {
public:
    uint8_t buffer[1024];

    EEPROMClass() {
        std::memset(buffer, 0xFF, sizeof(buffer)); // EEPROM defaults to 0xFF when erased
    }

    template<typename T>
    T& get(int idx, T& t) {
        std::memcpy(&t, &buffer[idx], sizeof(T));
        return t;
    }

    template<typename T>
    const T& put(int idx, const T& t) {
        std::memcpy(&buffer[idx], &t, sizeof(T));
        return t;
    }
};

extern EEPROMClass EEPROM;

#endif // MOCK_EEPROM_H
