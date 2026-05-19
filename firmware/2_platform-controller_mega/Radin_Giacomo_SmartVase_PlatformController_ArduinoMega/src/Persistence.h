#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <Arduino.h>
#include <EEPROM.h>
#include "smartvase_aliases.h"

class Persistence {
public:
    Persistence();
    void loadConfig();
    void saveConfig(bool force = false);
    void loadStats();
    void saveStats(bool force = false);
    DeviceConfig&    getConfig() { return config; }
    CumulativeStats& getStats()  { return stats; }

private:
    DeviceConfig    config;
    CumulativeStats stats;
    CumulativeStats lastSavedStats;
    unsigned long   lastEepromConfigWrite;
    unsigned long   lastEepromStatsWrite;
};

#endif // PERSISTENCE_H
