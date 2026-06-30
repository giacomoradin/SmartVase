/*!
    @file   Crc16.h

    @ingroup MegaComm

    @brief  CRC16-CCITT per il framing seriale Hub↔Mega e per i blob EEPROM.

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef CRC16_H
#define CRC16_H

#include <Arduino.h>

/*!
    @addtogroup MegaComm
    @{
*/

/*!
    @brief    Calcola il CRC16-CCITT di un buffer.

    @details  Polinomio `0x1021`, init `0x0000`, MSB-first, nessuna riflessione
              né xor-out finale. È l'implementazione condivisa (stesso
              algoritmo) usata sia dal framing del protocollo seriale verso
              l'Hub (`Communication.cpp`) sia dalla validazione dei blob in
              EEPROM (`Persistence.cpp`): deve restare bit-per-bit identica
              alla controparte lato Hub (`crc_utils.cpp`, variante CCITT) o i
              frame verranno scartati.

    @param[in] data   Puntatore al buffer su cui calcolare il CRC.
    @param[in] length Lunghezza del buffer, in byte.

    @return   Il CRC16 calcolato.
*/
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

/*! @} */ // MegaComm

#endif // CRC16_H
