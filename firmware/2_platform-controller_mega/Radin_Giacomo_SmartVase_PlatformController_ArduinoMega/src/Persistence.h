        #ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <Arduino.h>
#include <EEPROM.h>
#include "smartvase_aliases.h"

class Persistence {
public:
    Persistence();
    void loadConfig();
    void saveConfig();
    void loadStats();
    void saveStats(bool force = false);
    DeviceConfig& getConfig();
    CumulativeStats& getStats();

private:
    uint16_t crc16(const uint8_t* data, size_t length);

    DeviceConfig config;
    CumulativeStats stats;
    CumulativeStats lastSavedStats;
    unsigned long lastEepromConfigWrite;
    unsigned long lastEepromStatsWrite;
};

#endif // PERSISTENCE_H