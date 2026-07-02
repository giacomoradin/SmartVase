/*!
    @file   SystemStatus.h

    @ingroup MegaCore

    @brief  Shared system state (`struct SystemStatus`) and firmware version.

    @details `SystemStatus` is passed by reference to the modules that must
             read or update it (`Communication`, `main.cpp`, CLI): it is the
             coordination point between the Hub deadman, the RAM/sensor
             monitoring and the deferred EEPROM save.

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <Arduino.h>

/*!
    @addtogroup MegaCore
    @{
*/

/*! @brief Mega firmware version, logged at boot and shown by the CLI (`version`). */
#define SMARTVASE_FW_VERSION "5.3.0"

/*! @brief Maximum length (including terminator) of the `deviceId` buffer. */
#define DEVICE_ID_MAX_LEN 16

/*! @brief Maximum length (including terminator) of the `degradedReason` buffer. */
#define DEGRADED_REASON_MAX_LEN 32

/*!
    @struct SystemStatus
    @brief  Shared system state, read/updated by several modules.
*/
struct SystemStatus {
    bool bmeSensorError;       /**< true if the last BME680 reading failed (sensor not responding/absent). */
    bool lowMemoryDetected;    /**< true if the free SRAM dropped below the degraded-mode threshold. */
    bool degradedModeActive;   /**< true if the system is in degraded mode (low RAM, Hub missing, etc.): motors/pump disabled. */
    bool hubIsMissing;         /**< true if the Hub deadman has tripped (no message received for >120 s). */
    bool softResetRequested;   /**< true if a `softReset` command was received: the main loop will perform the reset on the next pass. */
    bool standaloneMode;       /**< Bench-test mode (CLI `standalone on`): suspends the Hub deadman so the Mega
                                     does not enter degraded mode when tested alone over USB. Not persisted: reverts to `false` on every reset. */
    bool configSavePending;    /**< Deferred EEPROM config save: set by Communication when a command modifies
                                     the config, executed by the main loop when the serial line is idle (avoids blocking the loop
                                     for ~60 ms during reception). */
    char degradedReason[DEGRADED_REASON_MAX_LEN];  /**< Textual description of the degraded-mode reason (for diagnostics/logs). */
    char deviceId[DEVICE_ID_MAX_LEN];               /**< Mega device identifier. */
};

/*! @} */ // MegaCore

#endif // SYSTEM_STATUS_H
