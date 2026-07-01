/*! @file crc_utils.h
 *  @ingroup HubSerial
 *  @brief Shared Hub CRC16 helpers: CCITT for the serial framing towards the
 *  Mega, IBM/ARC for the integrity of the configuration blobs in NVS.
 *  @details Previously duplicated in SerialManager and ConfigManager, now
 *  centralized here. The two polynomials are DIFFERENT on purpose, for two
 *  independent uses: they must not be swapped nor unified.
 *  @author Giacomo Radin
 *  @date 2026-06-30
 */

#ifndef CRC_UTILS_H
#define CRC_UTILS_H

#include <stdint.h>
#include <stddef.h>

/*! @addtogroup HubSerial
 *  @{
 */

/*! @brief CRC16-CCITT (poly 0x1021, init 0x0000, MSB-first, no reflect/xor-out).
 *  @details Used for the framing of the Hub<->Mega serial protocol
 *  (SerialManager). MUST stay bit-for-bit identical to `crc16_ccitt` of the
 *  Mega firmware (Crc16.cpp): a mismatch causes every frame to be discarded as
 *  a CRC mismatch. Pinned by the host test `test_crc_utils` (XMODEM vector, expected 0x31C3).
 *  @param[in] data Pointer to the buffer over which to compute the checksum.
 *  @param[in] length Number of bytes of `data` to include in the computation.
 *  @return CRC16 computed over the `length` bytes of `data`. */
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

/*! @brief CRC16-IBM/ARC (poly 0xA001, init 0x0000, LSB-first).
 *  @details Used for the integrity of the `DeviceConfig` blob stored in NVS
 *  (ConfigManager): purely Hub-internal use, the value is never exchanged with
 *  the Mega or the cloud. Pinned by the host test `test_crc_utils`
 *  (expected 0xBB3D).
 *  @param[in] data Pointer to the buffer over which to compute the checksum.
 *  @param[in] length Number of bytes of `data` to include in the computation.
 *  @return CRC16 computed over the `length` bytes of `data`. */
uint16_t crc16_ibm(const uint8_t* data, size_t length);

/*! @} */ // end of HubSerial group

#endif // CRC_UTILS_H
