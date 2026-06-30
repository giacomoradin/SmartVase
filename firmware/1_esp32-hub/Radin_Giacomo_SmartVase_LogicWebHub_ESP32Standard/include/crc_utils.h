/*! @file crc_utils.h
 *  @ingroup HubSerial
 *  @brief Utility CRC16 condivise dell'Hub: CCITT per il framing seriale verso
 *  il Mega, IBM/ARC per l'integrita' dei blob di configurazione in NVS.
 *  @details Prima duplicate in SerialManager e ConfigManager, centralizzate
 *  qui. I due polinomi sono DIVERSI di proposito, per due usi indipendenti:
 *  non vanno scambiati tra loro ne' unificati.
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
 *  @details Usato per il framing del protocollo seriale Hub<->Mega
 *  (SerialManager). DEVE restare bit-per-bit identico a `crc16_ccitt` del
 *  firmware Mega (Crc16.cpp): un disallineamento fa scartare ogni frame come
 *  CRC-mismatch. Pinnato dal test host `test_crc_utils` (vettore XMODEM, atteso 0x31C3).
 *  @param[in] data Puntatore al buffer su cui calcolare il checksum.
 *  @param[in] length Numero di byte di `data` da includere nel calcolo.
 *  @return CRC16 calcolato sui `length` byte di `data`. */
uint16_t crc16_ccitt(const uint8_t* data, size_t length);

/*! @brief CRC16-IBM/ARC (poly 0xA001, init 0x0000, LSB-first).
 *  @details Usato per l'integrita' del blob `DeviceConfig` salvato in NVS
 *  (ConfigManager): uso puramente interno dell'Hub, il valore non viene mai
 *  scambiato con il Mega o col cloud. Pinnato dal test host `test_crc_utils`
 *  (atteso 0xBB3D).
 *  @param[in] data Puntatore al buffer su cui calcolare il checksum.
 *  @param[in] length Numero di byte di `data` da includere nel calcolo.
 *  @return CRC16 calcolato sui `length` byte di `data`. */
uint16_t crc16_ibm(const uint8_t* data, size_t length);

/*! @} */ // end of HubSerial group

#endif // CRC_UTILS_H
