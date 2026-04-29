#include "Persistence.h"

#define EEPROM_MAGIC_NUMBER_STATS  0x18BEEF18
#define EEPROM_MAGIC_NUMBER_CONFIG 0xCF6BEEF6
#define EEPROM_CONFIG_SLOT_0_ADDR 0
#define EEPROM_CONFIG_SLOT_1_ADDR 50
#define EEPROM_STATS_SLO_0_ADDR 100
#define EEPROM_STATS_SLOT_1_ADDR 200
#define EEPROM_STATS_WRITE_INTERVAL 300000
#define EEPROM_CONFIG_WRITE_INTERVAL 60000

Persistence::Persistence() {
    this->lastEepromConfigWrite = 0;
    this->lastEepromStatsWrite = 0;
}

void Persistence::loadConfig() {
    DeviceConfig c0, c1;
    EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
    EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
    uint16_t crc0_calc = crc16((uint8_t*)&c0, sizeof(c0) - sizeof(c0.crc16));
    uint16_t crc1_calc = crc16((uint8_t*)&c1, sizeof(c1) - sizeof(c1.crc16));
    if (c0.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c0.crc16 == crc0_calc) { this->config = c0; /*LOG_INFO("eeprom_load", "Config slot 0 OK");*/ }
    else if (c1.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c1.crc16 == crc1_calc) { this->config = c1; /*LOG_INFO("eeprom_load", "Config slot 1 OK");*/ }
    else { this->config = {EEPROM_MAGIC_NUMBER_CONFIG, 0, 255, 240, 1000, 1200}; /*LOG_WARN("eeprom_load", "Config invalid, using defaults");*/ saveConfig(); }
}

void Persistence::saveConfig() {
    if (millis() - this->lastEepromConfigWrite < EEPROM_CONFIG_WRITE_INTERVAL) return;
    this->config.crc16 = crc16((uint8_t*)&this->config, sizeof(this->config) - sizeof(this->config.crc16));
    static uint8_t slot = 0;
    slot = (slot + 1) % 2;
    EEPROM.put(slot == 0 ? EEPROM_CONFIG_SLOT_0_ADDR : EEPROM_CONFIG_SLOT_1_ADDR, this->config);
    this->lastEepromConfigWrite = millis();
}

void Persistence::loadStats() {
    CumulativeStats s0, s1;
    EEPROM.get(EEPROM_STATS_SLO_0_ADDR, s0);
    EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);
    uint16_t crc0_calc = crc16((uint8_t*)&s0, sizeof(s0) - sizeof(s0.crc16));
    uint16_t crc1_calc = crc16((uint8_t*)&s1, sizeof(s1) - sizeof(s1.crc16));
    if (s0.magic_number == EEPROM_MAGIC_NUMBER_STATS && s0.crc16 == crc0_calc) { this->stats = s0; /*LOG_INFO("eeprom_load", "Stats slot 0 OK");*/}
    else if (s1.magic_number == EEPROM_MAGIC_NUMBER_STATS && s1.crc16 == crc1_calc) { this->stats = s1; /*LOG_INFO("eeprom_load", "Stats slot 1 OK");*/}
    else { memset(&this->stats, 0, sizeof(this->stats)); this->stats.magic_number = EEPROM_MAGIC_NUMBER_STATS; /*LOG_WARN("eeprom_load", "Stats invalid, resetting");*/}
    this->lastSavedStats = this->stats;
}

void Persistence::saveStats(bool force) {
    if (!force && millis() - this->lastEepromStatsWrite < EEPROM_STATS_WRITE_INTERVAL) return;
    if (force || memcmp(&this->stats, &this->lastSavedStats, sizeof(this->stats)) != 0) {
        this->stats.crc16 = crc16((uint8_t*)&this->stats, sizeof(this->stats) - sizeof(this->stats.crc16));
        static uint8_t slot = 0;
        slot = (slot + 1) % 2;
        EEPROM.put(slot == 0 ? EEPROM_STATS_SLO_0_ADDR : EEPROM_STATS_SLOT_1_ADDR, this->stats);
        this->lastSavedStats = this->stats;
        this->lastEepromStatsWrite = millis();
    }
}

DeviceConfig& Persistence::getConfig() {
    return this->config;
}

CumulativeStats& Persistence::getStats() {
    return this->stats;
}

uint16_t Persistence::crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0x0;
    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
