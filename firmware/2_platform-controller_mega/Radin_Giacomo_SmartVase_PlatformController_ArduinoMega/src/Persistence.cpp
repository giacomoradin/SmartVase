/*!
    @file   Persistence.cpp

    @ingroup MegaPersistence

    @brief  Implementation of the dual-slot EEPROM persistence (config + stats).

    @date   2026-04-29

    @author Giacomo Radin
*/

#include "Persistence.h"
#include "Crc16.h"
#include "CarePolicy.h"

// Guards: if adding fields makes a struct exceed its slot's space, it would
// overlap with the next one (a silent bug). These fail at compile time.
static_assert(sizeof(DeviceConfig)    <= 64,  "DeviceConfig exceeds the EEPROM slot (64 B)");
static_assert(sizeof(CumulativeStats) <= 128, "CumulativeStats exceeds the EEPROM slot (128 B)");

// =================================================================
// Dual-slot EEPROM persistence (wear leveling) with magic+CRC16.
// =================================================================

/*! @name Magic numbers to recognize a valid EEPROM slot
 *  @{ */
#define EEPROM_MAGIC_NUMBER_CONFIG 0xCF6BEEF6  ///< Magic for the configuration slots.
#define EEPROM_MAGIC_NUMBER_STATS  0x18BEEF18  ///< Magic for the statistics slots.
/*! @} */

/*! @name Addresses of the two dual-slot pairs in EEPROM
 *  @details Generous margin, since sizeof(DeviceConfig) grows with new fields
 *           (see static_assert above).
 *  @{ */
#define EEPROM_CONFIG_SLOT_0_ADDR    0    ///< Configuration slot 0.
#define EEPROM_CONFIG_SLOT_1_ADDR   64    ///< Configuration slot 1.
#define EEPROM_STATS_SLOT_0_ADDR   128    ///< Statistics slot 0.
#define EEPROM_STATS_SLOT_1_ADDR   256    ///< Statistics slot 1.
/*! @} */

/*! @name EEPROM write throttling (anti-wear)
 *  @{ */
#define EEPROM_CONFIG_WRITE_INTERVAL  60000UL    ///< Minimum interval between two config writes (ms).
#define EEPROM_STATS_WRITE_INTERVAL  300000UL    ///< Minimum interval between two stats writes (ms).
/*! @} */

/*!
    @brief    Calculates the CRC16 of a DeviceConfig, excluding its own crc16 field.

    @details  The blob CONTAINS its own crc16 field, so the calculation must be done on a
              local copy with that field zeroed out, to cover the entire struct deterministically.
              (Historical bug fixed: the previous version included in the
              calculation the crc16 with the old value and excluded the last 2 bytes of data,
              so the verification at load always failed and config/stats reverted to
              defaults at each boot.)

    @param[in] c Config to calculate the CRC for (passed by copy: the
                 crc16 field of the original is untouched).

    @return   The CRC16-CCITT of the struct with crc16 zeroed.
*/
static uint16_t configCrc(DeviceConfig c) {
    c.crc16 = 0;
    return crc16_ccitt((uint8_t*)&c, sizeof(c));
}

/*!
    @brief    Calculates the CRC16 of a CumulativeStats, excluding its own crc16 field.
    @details  Same logic as ::configCrc, applied to cumulative statistics.
    @param[in] s Statistics to calculate the CRC for (passed by copy).
    @return   The CRC16-CCITT of the struct with crc16 zeroed.
*/
static uint16_t statsCrc(CumulativeStats s) {
    s.crc16 = 0;
    return crc16_ccitt((uint8_t*)&s, sizeof(s));
}

Persistence::Persistence()
    : lastEepromConfigWrite(0), lastEepromStatsWrite(0),
      current_config_slot(0), current_stats_slot(0)
{
    memset(&config, 0, sizeof(config));
    memset(&stats, 0, sizeof(stats));
    memset(&lastSavedStats, 0, sizeof(lastSavedStats));
}


void Persistence::loadConfig() {
    DeviceConfig c0, c1;
    EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
    EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);

    bool c0_valid = (c0.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c0.crc16 == configCrc(c0));
    bool c1_valid = (c1.magic_number == EEPROM_MAGIC_NUMBER_CONFIG && c1.crc16 == configCrc(c1));

    if (c0_valid && c1_valid) {
        if (c0.write_counter >= c1.write_counter) {
            config = c0;
            current_config_slot = 0;
        } else {
            config = c1;
            current_config_slot = 1;
        }
    } else if (c0_valid) {
        config = c0;
        current_config_slot = 0;
    } else if (c1_valid) {
        config = c1;
        current_config_slot = 1;
    } else {
        // Default factory.
        config.magic_number      = EEPROM_MAGIC_NUMBER_CONFIG;
        config.crc16             = 0;
        config.write_counter     = 0;
        config.motorCalibLeft    = 255;
        config.motorCalibRight   = 240;
        config.avoid_reverse_ms  = 1000;
        config.avoid_turn_ms     = 1200;
        config.soil_dry_threshold = 450;
        // Bench-calibrated 2026-06-30: LDR covered (dark) ADC~11, with lab
        // fluorescent lights on ADC~539, on a real scale of ~0-800 (not 0-1023).
        config.light_threshold    = 500;
        // Conservative default, to be tuned on the bench with `tank <cm>` (CLI):
        // US4->water distance beyond which the pump is blocked.
        config.tank_empty_cm      = 20;
        // Autonomous care defaults: MEDIUM plant preset, care DISABLED until
        // explicitly enabled from the CLI (`care on`) — a freshly flashed robot
        // must never start moving/watering on its own on the bench.
        {
            const PlantProfile mp = carePresetProfile(CARE_PLANT_MEDIUM);
            config.care_enabled           = 0;
            config.care_plant_kind        = CARE_PLANT_MEDIUM;
            config.care_light_target_min  = mp.light_target_min;
            config.care_lux_high_adc      = mp.lux_high_adc;
            config.care_soil_wet_adc      = mp.soil_wet_adc;
            config.care_dose_ms           = mp.dose_ms;
            config.care_soak_min          = mp.soak_min;
            config.care_max_doses         = mp.max_doses_per_day;
            config.care_max_reloc         = mp.max_reloc_per_day;
            config.care_growlight_max_min = mp.grow_light_max_min;
        }
        current_config_slot      = 0;

        config.crc16 = configCrc(config);
        EEPROM.put(EEPROM_CONFIG_SLOT_0_ADDR, config);
        lastEepromConfigWrite = millis();
    }
}

void Persistence::saveConfig(bool force) {
    if (!force && (millis() - lastEepromConfigWrite < EEPROM_CONFIG_WRITE_INTERVAL)) return;
    config.write_counter++;
    config.crc16 = configCrc(config);
    uint8_t next_slot = (current_config_slot == 0) ? 1 : 0;
    EEPROM.put(next_slot == 0 ? EEPROM_CONFIG_SLOT_0_ADDR : EEPROM_CONFIG_SLOT_1_ADDR, config);
    current_config_slot = next_slot;
    lastEepromConfigWrite = millis();
}

void Persistence::loadStats() {
    CumulativeStats s0, s1;
    EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
    EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);

    bool s0_valid = (s0.magic_number == EEPROM_MAGIC_NUMBER_STATS && s0.crc16 == statsCrc(s0));
    bool s1_valid = (s1.magic_number == EEPROM_MAGIC_NUMBER_STATS && s1.crc16 == statsCrc(s1));

    if (s0_valid && s1_valid) {
        if (s0.write_counter >= s1.write_counter) {
            stats = s0;
            current_stats_slot = 0;
        } else {
            stats = s1;
            current_stats_slot = 1;
        }
    } else if (s0_valid) {
        stats = s0;
        current_stats_slot = 0;
    } else if (s1_valid) {
        stats = s1;
        current_stats_slot = 1;
    } else {
        memset(&stats, 0, sizeof(stats));
        stats.magic_number = EEPROM_MAGIC_NUMBER_STATS;
        stats.write_counter = 0;
        current_stats_slot = 0;
    }
    lastSavedStats = stats;
}

void Persistence::saveStats(bool force) {
    if (!force && (millis() - lastEepromStatsWrite < EEPROM_STATS_WRITE_INTERVAL)) return;
    if (!force && memcmp(&stats, &lastSavedStats, sizeof(stats)) == 0) return;

    stats.write_counter++;
    stats.crc16 = statsCrc(stats);
    uint8_t next_slot = (current_stats_slot == 0) ? 1 : 0;
    EEPROM.put(next_slot == 0 ? EEPROM_STATS_SLOT_0_ADDR : EEPROM_STATS_SLOT_1_ADDR, stats);
    current_stats_slot = next_slot;
    lastSavedStats = stats;
    lastEepromStatsWrite = millis();
}
