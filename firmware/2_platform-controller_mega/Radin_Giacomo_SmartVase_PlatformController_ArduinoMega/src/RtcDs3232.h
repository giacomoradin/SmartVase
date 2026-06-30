/*!
 * @file RtcDs3232.h
 * @ingroup MegaSensors
 * @brief Driver locale I2C per RTC DS3232/DS3231 (lettura/scrittura epoch UNIX, flag oscillatore fermo).
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#ifndef RTC_DS3232_H
#define RTC_DS3232_H

#include <Arduino.h>
#include <TimeLib.h>

/**
 * @class RtcDs3232
 * @brief Driver minimale per RTC DS3232/DS3231 su I2C (indirizzo 0x68).
 *
 * Implementato in locale al posto della libreria jchristensen/DS3232RTC per eliminare la dipendenza dal
 * registry PlatformIO (macchina di sviluppo offline): espone solo le operazioni usate dal firmware
 * (lettura/scrittura epoch UNIX + flag Oscillator-Stop).
 */
class RtcDs3232 {
public:
    /**
     * @brief Verifica la presenza del chip sul bus I2C.
     * @return true se il device risponde all'indirizzo 0x68.
     * @note Richiede che `Wire.begin()` sia gia' stato chiamato dal chiamante (Sensors::init).
     */
    bool begin();

    /**
     * @brief Legge l'ora corrente dal chip e la converte in epoch UNIX.
     * @return Timestamp UNIX in secondi, oppure 0 se il chip non risponde sul bus I2C.
     * @note L'anno e' memorizzato sul chip come 0-99 (offset 2000); la conversione assume sempre il
     *       secolo 2000-2099.
     */
    time_t get();

    /**
     * @brief Scrive l'epoch UNIX indicato sul chip e azzera il flag Oscillator-Stop (OSF).
     * @param[in] t Timestamp UNIX in secondi da impostare.
     * @return true se entrambe le transazioni I2C (scrittura ora + azzeramento OSF) sono andate a buon fine,
     *         false su qualunque errore I2C.
     */
    bool set(time_t t);

    /**
     * @brief Verifica se l'oscillatore del chip si e' fermato dall'ultimo `set()`.
     * @return true se l'oscillatore risulta fermo (batteria tampone scarica o assente: l'ora letta da
     *         `get()` non e' affidabile) oppure se la lettura I2C dello status fallisce (fail-safe).
     */
    bool oscillatorStopped();

private:
    /** @brief Converte un byte BCD (Binary-Coded Decimal) in valore decimale. */
    uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
    /** @brief Converte un valore decimale (0-99) nel formato BCD usato dai registri del DS3232. */
    uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
};

#endif // RTC_DS3232_H
