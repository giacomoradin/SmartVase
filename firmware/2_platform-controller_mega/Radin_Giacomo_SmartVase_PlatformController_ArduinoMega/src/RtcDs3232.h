#ifndef RTC_DS3232_H
#define RTC_DS3232_H

#include <Arduino.h>
#include <TimeLib.h>

// Driver minimale per RTC DS3232/DS3231 su I2C (indirizzo 0x68).
//
// Implementato in locale al posto della libreria jchristensen/DS3232RTC
// per eliminare la dipendenza dal registry PlatformIO: espone solo le
// operazioni usate dal firmware (lettura/scrittura epoch + flag OSF).
class RtcDs3232 {
public:
    // Verifica la presenza del chip sul bus. Wire.begin() va chiamato prima.
    // Ritorna true se il device risponde all'indirizzo 0x68.
    bool begin();

    // Epoch Unix (s). Ritorna 0 se il chip non risponde.
    time_t get();

    // Scrive l'epoch e azzera il flag Oscillator-Stop (OSF).
    // Ritorna false su errore I2C.
    bool set(time_t t);

    // true se l'oscillatore si e' fermato dall'ultimo set():
    // l'ora letta non e' affidabile (batteria tampone scarica/assente).
    bool oscillatorStopped();

private:
    uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
    uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
};

#endif // RTC_DS3232_H
