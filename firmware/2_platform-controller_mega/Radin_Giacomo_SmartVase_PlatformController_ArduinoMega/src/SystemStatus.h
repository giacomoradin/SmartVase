/*!
    @file   SystemStatus.h

    @ingroup MegaCore

    @brief  Stato condiviso del sistema (`struct SystemStatus`) e versione firmware.

    @details `SystemStatus` è passata per riferimento ai moduli che devono
             leggerla o aggiornarla (`Communication`, `main.cpp`, CLI): è il
             punto di coordinamento tra il deadman dell'Hub, il monitoraggio
             RAM/sensori e il salvataggio EEPROM deferito.

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

/*! @brief Versione del firmware Mega, loggata al boot e mostrata dalla CLI (`version`). */
#define SMARTVASE_FW_VERSION "5.2.0"

/*! @brief Lunghezza massima (incluso terminatore) del buffer `deviceId`. */
#define DEVICE_ID_MAX_LEN 16

/*! @brief Lunghezza massima (incluso terminatore) del buffer `degradedReason`. */
#define DEGRADED_REASON_MAX_LEN 32

/*!
    @struct SystemStatus
    @brief  Stato condiviso del sistema, letto/aggiornato da più moduli.
*/
struct SystemStatus {
    bool bmeSensorError;       /**< true se l'ultima lettura del BME680 è fallita (sensore non risponde/assente). */
    bool lowMemoryDetected;    /**< true se la SRAM libera è scesa sotto la soglia di degraded mode. */
    bool degradedModeActive;   /**< true se il sistema è in modalità degradata (RAM bassa, Hub assente, ecc.): motori/pompa disabilitati. */
    bool hubIsMissing;         /**< true se il deadman dell'Hub è scattato (nessun messaggio ricevuto da >120 s). */
    bool softResetRequested;   /**< true se è stato ricevuto un comando `softReset`: il main loop eseguirà il reset al prossimo giro. */
    bool standaloneMode;       /**< Modalità banco di prova (CLI `standalone on`): sospende il deadman dell'Hub così il Mega
                                     non entra in degraded mode quando viene testato da solo via USB. Non persistita: torna `false` a ogni reset. */
    bool configSavePending;    /**< Salvataggio config EEPROM deferito: impostato da Communication quando un comando modifica
                                     la config, eseguito dal main loop quando la seriale è a riposo (evita di bloccare il loop
                                     per ~60 ms durante la ricezione). */
    char degradedReason[DEGRADED_REASON_MAX_LEN];  /**< Descrizione testuale del motivo di degraded mode (per diagnostica/log). */
    char deviceId[DEVICE_ID_MAX_LEN];               /**< Identificativo del device Mega. */
};

/*! @} */ // MegaCore

#endif // SYSTEM_STATUS_H
