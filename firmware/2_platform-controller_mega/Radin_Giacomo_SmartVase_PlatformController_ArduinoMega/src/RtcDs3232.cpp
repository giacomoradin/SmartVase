/*!
 * @file RtcDs3232.cpp
 * @ingroup MegaSensors
 * @brief Implementation of the DS3232 driver: I2C transactions for reading/writing the time and handling the OSF flag.
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#include "RtcDs3232.h"
#include <Wire.h>

#define DS3232_I2C_ADDR   0x68 /**< 7-bit I2C address of DS3232/DS3231. */
#define DS3232_REG_TIME   0x00 /**< Initial register of the time block: 7 BCD bytes (sec, min, hour, dow, day, month, year). */
#define DS3232_REG_STATUS 0x0F /**< Status register; bit7 = OSF (Oscillator Stop Flag). */

bool RtcDs3232::begin() {
    Wire.beginTransmission(DS3232_I2C_ADDR);
    return Wire.endTransmission() == 0;
}

time_t RtcDs3232::get() {
    Wire.beginTransmission(DS3232_I2C_ADDR);
    Wire.write((uint8_t)DS3232_REG_TIME);
    if (Wire.endTransmission() != 0) return 0;
    if (Wire.requestFrom((uint8_t)DS3232_I2C_ADDR, (uint8_t)7) != 7) return 0;

    tmElements_t tm;
    tm.Second = bcd2dec(Wire.read() & 0x7F);
    tm.Minute = bcd2dec(Wire.read());
    tm.Hour   = bcd2dec(Wire.read() & 0x3F); // 24h format
    tm.Wday   = Wire.read();                  // 1-7, not used by makeTime
    tm.Day    = bcd2dec(Wire.read());
    tm.Month  = bcd2dec(Wire.read() & 0x1F); // bit7 = century, ignored
    // Year 0-99 = 2000-2099; TimeLib offset is from 1970.
    tm.Year   = bcd2dec(Wire.read()) + 30;

    return makeTime(tm);
}

bool RtcDs3232::set(time_t t) {
    tmElements_t tm;
    breakTime(t, tm);

    Wire.beginTransmission(DS3232_I2C_ADDR);
    Wire.write((uint8_t)DS3232_REG_TIME);
    Wire.write(dec2bcd(tm.Second));
    Wire.write(dec2bcd(tm.Minute));
    Wire.write(dec2bcd(tm.Hour));
    Wire.write(tm.Wday);
    Wire.write(dec2bcd(tm.Day));
    Wire.write(dec2bcd(tm.Month));
    Wire.write(dec2bcd(tm.Year - 30));
    if (Wire.endTransmission() != 0) return false;

    // Clear OSF preserving other status bits.
    Wire.beginTransmission(DS3232_I2C_ADDR);
    Wire.write((uint8_t)DS3232_REG_STATUS);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)DS3232_I2C_ADDR, (uint8_t)1) != 1) return false;
    uint8_t status = Wire.read();

    Wire.beginTransmission(DS3232_I2C_ADDR);
    Wire.write((uint8_t)DS3232_REG_STATUS);
    Wire.write(status & ~0x80);
    return Wire.endTransmission() == 0;
}

bool RtcDs3232::oscillatorStopped() {
    Wire.beginTransmission(DS3232_I2C_ADDR);
    Wire.write((uint8_t)DS3232_REG_STATUS);
    if (Wire.endTransmission() != 0) return true;
    if (Wire.requestFrom((uint8_t)DS3232_I2C_ADDR, (uint8_t)1) != 1) return true;
    return (Wire.read() & 0x80) != 0;
}
