/*!
    @file   Persistence.h

    @ingroup MegaPersistence

    @brief  Dual-slot (wear-leveling) EEPROM persistence of config and statistics.

    @date   2026-04-29

    @author Giacomo Radin
*/

#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <Arduino.h>
#include <EEPROM.h>
#include "smartvase_aliases.h"

/*!
    @addtogroup MegaPersistence
    @{
*/

/*!
    @class Persistence
    @brief Manager for persisting configuration and statistics data to the Arduino Mega EEPROM.

    @details Implements a dual-slot mechanism to prevent data corruption on power loss during a write
             (wear-leveling and fail-safe), protected by CRC16 (Crc16.h) and a magic number.
             On load it picks the valid slot with the highest `write_counter`; on save it always writes
             to the slot opposite the current one. It also implements time-based write throttling
             (`saveConfig`/`saveStats`, see the `EEPROM_*_WRITE_INTERVAL` constants in Persistence.cpp) to
             avoid prematurely wearing out the EEPROM, with the option to force an immediate write.
*/
class Persistence {
public:
    /**
     * @brief Constructor of the Persistence class.
     */
    Persistence();

    /**
     * @brief Loads the device configuration (thresholds, modes, etc.) from EEPROM.
     *
     * Reads both slots, validates them via CRC16, and picks the slot with the higher write counter.
     */
    void loadConfig();

    /**
     * @brief Saves the current configuration to EEPROM.
     *
     * Writes to the slot alternate to the last loaded one, to spread out wear.
     *
     * @param force If set to true, ignores the time-based throttling (default: false).
     */
    void saveConfig(bool force = false);

    /**
     * @brief Loads the device's cumulative statistics from EEPROM.
     */
    void loadStats();

    /**
     * @brief Saves the current cumulative statistics to EEPROM.
     *
     * @param force If set to true, ignores the time-based throttling (default: false).
     */
    void saveStats(bool force = false);

    /**
     * @brief Returns a reference to the current configuration.
     * @return DeviceConfig& Device configuration.
     */
    DeviceConfig&    getConfig() { return config; }

    /**
     * @brief Returns a reference to the current cumulative statistics.
     * @return CumulativeStats& Usage statistics.
     */
    CumulativeStats& getStats()  { return stats; }

private:
    DeviceConfig    config;             /**< Data structure holding the current configuration */
    CumulativeStats stats;              /**< Data structure holding the current statistics */
    CumulativeStats lastSavedStats;     /**< Copy of the last save, used to check for real changes */
    unsigned long   lastEepromConfigWrite; /**< Timestamp (in ms) of the last configuration write */
    unsigned long   lastEepromStatsWrite;  /**< Timestamp (in ms) of the last statistics write */
    uint8_t         current_config_slot;   /**< Active configuration slot (0 or 1) */
    uint8_t         current_stats_slot;    /**< Active statistics slot (0 or 1) */
};

/*! @} */ // MegaPersistence

#endif // PERSISTENCE_H
