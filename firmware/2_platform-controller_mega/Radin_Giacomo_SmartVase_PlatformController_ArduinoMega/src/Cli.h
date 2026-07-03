/*!
    @file   Cli.h

    @ingroup MegaCli

    @brief  Debug serial CLI of the Mega, on the USB port.

    @details Uses `Serial` (115200 baud, `\n` terminator) and does not interfere
             with the serial link toward the Hub (`Serial1`, see `Communication`).
             Available commands:

             | Command                    | Effect |
             |-----------------------------|---------|
             | `help` / `?`                | Command list |
             | `version`                   | Firmware version |
             | `status`                    | Operating mode + runtime state + RAM |
             | `stats`                     | Cumulative statistics from EEPROM |
             | `config`                    | Current configuration |
             | `sensors`                   | Latest sensor readings |
             | `diag`                      | Guided diagnostics: for each sensor/motor, state + what to check if it does not work |
             | `i2cscan`                   | Hardware I²C bus scan (pins 20/21) with hints on the expected devices (RTC 0x68, HW-084 module EEPROM 0x50-0x57, BME680 0x76/0x77) |
             | `stats reset`               | Clears the cumulative EEPROM statistics (fresh baseline after debug sessions) |
             | `tank`                      | Tank status (level, threshold, verdict) |
             | `tank <cm>`                 | Sets the tank-empty threshold (persisted) |
             | `light <adc>`               | Sets the brightness threshold 0..1023 (persisted): used both by LIGHT/SHADOW seeking and by the UVA lights |
             | `rtc`                       | Current epoch + oscillator validity (real chip or software clock) |
             | `rtc set <epoch>`           | Sets the time (Unix epoch in s); if the DS3232 chip does not respond, activates a software fallback clock (`millis()`-based, lost on reset) |
             | `mode <idle\|light\|shadow>` | Operating mode change |
             | `plant`                     | Current plant profile (preset, light target, soil thresholds, doses) |
             | `plant <shade\|medium\|sun>` | Applies a plant preset (persisted) |
             | `care`                      | Autonomous care layer status: FSM state, light budget, doses, relocations |
             | `care <on\|off>`            | Enables/disables autonomous care (persisted; default off) |
             | `wall <left\|right\|off>`   | Local side wall-following (US5/US6); overrides seeking while active |
             | `motor <f\|b\|l\|r> <ms>`    | Motor test (max 60000 ms, wheels lifted) |
             | `mfp0`                      | Continuous forward motor test (10 min, any key stops) for electrical checks |
             | `motortest`                 | Guided f/b/l/r sequence to verify directions/mapping |
             | `calib <left> <right>`      | Motor PWM calibration 0..255 (persisted) |
             | `pump <ms>`                 | Pump test (max 60000 ms, blocked if the tank is empty) |
             | `standalone <on\|off>`       | Suspends the Hub deadman for bench tests |
             | `reboot`                    | Soft reset via WDT |

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef CLI_H
#define CLI_H

#include <Arduino.h>

class Movement;
class Sensors;
class Pump;
class GrowLight;
class Persistence;
class Care;
struct SystemStatus;

/*!
    @addtogroup MegaCli
    @{
*/

/*!
    @class  Cli
    @brief  Parser and dispatcher of the USB serial CLI commands (see the command table in Cli.h).
*/
class Cli {
public:
    /*! @brief Constructor: initializes the empty line buffer. */
    Cli();

    /*!
        @brief    Reads the available characters from `Serial` without blocking and, at
                   end of line (`\n`), executes the corresponding command.
        @details  To be called on every main-loop iteration. Accumulates the characters
                   in a fixed buffer (`BUF_SIZE`) until the terminator is received.
        @param[in,out] mv  State/commands of the Movement module.
        @param[in,out] sn  State/readings of the Sensors module.
        @param[in,out] pp  State/commands of the Pump module.
        @param[in,out] gl  State of the grow-light relay (GrowLight).
        @param[in,out] cr  Autonomous care layer (Care), for the `plant`/`care` commands.
        @param[in,out] ps  Persisted configuration/statistics (Persistence).
        @param[in,out] sys Shared system state (SystemStatus).
    */
    void tick(Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl, Care& cr,
              Persistence& ps, SystemStatus& sys);

private:
    /*! @brief Parses a complete command line and executes its action. */
    void execute(const char* line, Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl,
                 Care& cr, Persistence& ps, SystemStatus& sys);
    /*! @brief Prints the list of available commands (`help`/`?` command). */
    void printHelp();
    /*! @brief Prints operating mode, runtime state and free RAM (`status` command). */
    void printStatus(Movement& mv, Pump& pp, GrowLight& gl, Care& cr, SystemStatus& sys);
    /*! @brief Prints the current plant profile (`plant` command). */
    void printPlant(Persistence& ps);
    /*! @brief Prints the autonomous-care status and daily KPIs (`care` command). */
    void printCare(Care& cr, Persistence& ps);
    /*! @brief Prints the cumulative statistics read from EEPROM (`stats` command). */
    void printStats(Persistence& ps);
    /*! @brief Prints the current configuration read from EEPROM (`config` command). */
    void printConfig(Persistence& ps);
    /*! @brief Prints the latest sensor readings (`sensors` command). */
    void printSensors(Sensors& sn);
    /*! @brief Runs the guided sensor/motor diagnostics, with troubleshooting hints (`diag` command). */
    void printDiag(Sensors& sn, Movement& mv, Pump& pp, GrowLight& gl, Persistence& ps, SystemStatus& sys);
    /*! @brief Prints the tank status (level, threshold, empty/full verdict) (`tank` command). */
    void printTank(Sensors& sn, Pump& pp, Persistence& ps);

    static const size_t BUF_SIZE = 64;  /**< Size of the CLI line accumulation buffer. */
    char  buf[BUF_SIZE];                /**< Buffer of received characters, waiting for the terminator. */
    size_t pos;                         /**< Current write position in buf. */
};

/*! @} */ // MegaCli

#endif // CLI_H
