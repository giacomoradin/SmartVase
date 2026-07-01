/*!
    @file   Crc16.h

    @ingroup MegaComm

    @brief  CRC16-CCITT for the Hub<->Mega serial framing and for EEPROM blobs.

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
    @brief    Computes the CRC16-CCITT of a buffer.

    @details  Polynomial `0x1021`, init `0x0000`, MSB-first, no reflection and
              no final xor-out. This is the shared implementation (same
              algorithm) used both by the serial protocol framing towards the
              Hub (`Communication.cpp`) and by the validation of the EEPROM
              blobs (`Persistence.cpp`): it must stay bit-for-bit identical to
              the Hub-side counterpart (`crc_utils.cpp`, CCITT variant) or the
              frames will be discarded.

    @param[in] data   Pointer to the buffer the CRC is computed over.
    @param[in] length Length of the buffer, in bytes.

    @return   The computed CRC16.
*/
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

/*! @} */ // MegaComm

#endif // CRC16_H
