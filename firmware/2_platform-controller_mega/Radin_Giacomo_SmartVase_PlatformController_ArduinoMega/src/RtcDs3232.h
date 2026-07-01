/*!
 * @file RtcDs3232.h
 * @ingroup MegaSensors
 * @brief Local I2C driver for the DS3232/DS3231 RTC (UNIX epoch read/write, oscillator-stopped flag).
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#ifndef RTC_DS3232_H
#define RTC_DS3232_H

#include <Arduino.h>
#include <TimeLib.h>

/**
 * @class RtcDs3232
 * @brief Minimal driver for the DS3232/DS3231 RTC over I2C (address 0x68).
 *
 * Implemented locally instead of the jchristensen/DS3232RTC library to remove the dependency on the
 * PlatformIO registry (offline development machine): it exposes only the operations used by the firmware
 * (UNIX epoch read/write + Oscillator-Stop flag).
 */
class RtcDs3232 {
public:
    /**
     * @brief Checks for the chip's presence on the I2C bus.
     * @return true if the device answers at address 0x68.
     * @note Requires that `Wire.begin()` has already been called by the caller (Sensors::init).
     */
    bool begin();

    /**
     * @brief Reads the current time from the chip and converts it to a UNIX epoch.
     * @return UNIX timestamp in seconds, or 0 if the chip does not answer on the I2C bus.
     * @note The year is stored on the chip as 0-99 (offset 2000); the conversion always assumes the
     *       2000-2099 century.
     */
    time_t get();

    /**
     * @brief Writes the given UNIX epoch to the chip and clears the Oscillator-Stop flag (OSF).
     * @param[in] t UNIX timestamp in seconds to set.
     * @return true if both I2C transactions (time write + OSF clear) succeeded,
     *         false on any I2C error.
     */
    bool set(time_t t);

    /**
     * @brief Checks whether the chip's oscillator has stopped since the last `set()`.
     * @return true if the oscillator is found stopped (backup battery flat or absent: the time read by
     *         `get()` is not reliable) or if the I2C read of the status register fails (fail-safe).
     */
    bool oscillatorStopped();

private:
    /** @brief Converts a BCD (Binary-Coded Decimal) byte to its decimal value. */
    uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
    /** @brief Converts a decimal value (0-99) into the BCD format used by the DS3232 registers. */
    uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
};

#endif // RTC_DS3232_H
