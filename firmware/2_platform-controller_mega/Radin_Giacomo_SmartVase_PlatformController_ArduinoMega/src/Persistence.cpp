#include "Persistence.h"
#include "Crc16.h"

// =================================================================
// Persistenza EEPROM dual-slot (wear leveling) con magic+CRC16.
// =================================================================

// Magic numbers per riconoscere slot validi
#define EEPROM_MAGIC_NUMBER_CONFIG 0xCF6BEEF6
#define EEPROM_MAGIC_NUMBER_STATS  0x18BEEF18

// Indirizzi dei due slot di config (sizeof(DeviceConfig) cresce con i nuovi campi,
// quindi tengo abbondante margine).
#define EEPROM_CONFIG_SLOT_0_ADDR    0
#define EEPROM_CONFIG_SLOT_1_ADDR   64
#define EEPROM_STATS_SLOT_0_ADDR   128
#define EEPROM_STATS_SLOT_1_ADDR   256

// Throttling scritture
#define EEPROM_CONFIG_WRITE_INTERVAL  60000UL    //  60 s
#define EEPROM_STATS_WRITE_INTERVAL  300000UL    // 300 s

Persistence::Persistence()
    : lastEepromConfigWrite(0), lastEepromStatsWrite(0)
{
    memset(&config, 0, sizeof(config));
    memset(&stats, 0, sizeof(stats));
    memset(&lastSavedStats, 0, sizeof(lastSavedStats));
}


void Persistence::loadConfig() {
    DeviceConfig c0, c1;
    EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
    EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
    uint16_t crc0_calc = crc16_ccitt((uint8_t*)&c0, sizeof(c0) - sizeof(c0.crc16));
    uint16_t crc1_calc = crc16_ccitt((uint8_t*)&c1, sizeof(c1) - sizeof(c1.crc16));

    if (c0.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c0.crc16 == crc0_calc) {
        config = c0;
    } else if (c1.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c1.crc16 == crc1_calc) {
        config = c1;
    } else {
        // Default factory.
        config.magic_number      = EEPROM_MAGIC_NUMBER_CONFIG;
        config.crc16             = 0;
        config.motorCalibLeft    = 255;
        config.motorCalibRight   = 240;
        config.avoid_reverse_ms  = 1000;
        config.avoid_turn_ms     = 1200;
        config.soil_dry_threshold = 450;
        config.light_threshold    = 600;
        // Default prudente da tarare a banco con `tank <cm>` (CLI):
        // distanza US4->acqua oltre la quale la pompa viene bloccata.
        config.tank_empty_cm      = 20;
        saveConfig(true);
    }
}

void Persistence::saveConfig(bool force) {
    if (!force && (millis() - lastEepromConfigWrite < EEPROM_CONFIG_WRITE_INTERVAL)) return;
    config.crc16 = crc16_ccitt((uint8_t*)&config, sizeof(config) - sizeof(config.crc16));
    static uint8_t slot = 0;
    slot = (slot + 1) % 2;
    EEPROM.put(slot == 0 ? EEPROM_CONFIG_SLOT_0_ADDR : EEPROM_CONFIG_SLOT_1_ADDR, config);
    lastEepromConfigWrite = millis();
}

void Persistence::loadStats() {
    CumulativeStats s0, s1;
    EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
    EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);
    uint16_t crc0_calc = crc16_ccitt((uint8_t*)&s0, sizeof(s0) - sizeof(s0.crc16));
    uint16_t crc1_calc = crc16_ccitt((uint8_t*)&s1, sizeof(s1) - sizeof(s1.crc16));

    if (s0.magic_number == EEPROM_MAGIC_NUMBER_STATS && s0.crc16 == crc0_calc) {
        stats = s0;
    } else if (s1.magic_number == EEPROM_MAGIC_NUMBER_STATS && s1.crc16 == crc1_calc) {
        stats = s1;
    } else {
        memset(&stats, 0, sizeof(stats));
        stats.magic_number = EEPROM_MAGIC_NUMBER_STATS;
    }
    lastSavedStats = stats;
}

void Persistence::saveStats(bool force) {
    if (!force && (millis() - lastEepromStatsWrite < EEPROM_STATS_WRITE_INTERVAL)) return;
    if (!force && memcmp(&stats, &lastSavedStats, sizeof(stats)) == 0) return;

    stats.crc16 = crc16_ccitt((uint8_t*)&stats, sizeof(stats) - sizeof(stats.crc16));
    static uint8_t slot = 0;
    slot = (slot + 1) % 2;
    EEPROM.put(slot == 0 ? EEPROM_STATS_SLOT_0_ADDR : EEPROM_STATS_SLOT_1_ADDR, stats);
    lastSavedStats = stats;
    lastEepromStatsWrite = millis();
}
