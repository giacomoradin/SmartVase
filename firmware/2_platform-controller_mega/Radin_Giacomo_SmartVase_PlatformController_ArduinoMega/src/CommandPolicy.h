/*!
    @file   CommandPolicy.h

    @ingroup MegaPolicy

    @brief  Politiche pure di validazione/clamp dei comandi ricevuti dall'Hub.

    @details Funzioni `static inline` senza alcuna dipendenza hardware (nessun
             `Arduino.h`), pensate per essere incluse sia dal firmware
             (`Communication.cpp`, in fase di `executeCommand`) sia dai test
             unitari a host in `tests/host/` (vedi `test_command_policy`).
             Implementano la difesa "defense-in-depth" lato Mega: anche se
             l'Hub a monte già valida/clampa i comandi, il Mega non si fida
             ciecamente e riapplica gli stessi vincoli prima di agire su
             motori/pompa/EEPROM.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef COMMAND_POLICY_H
#define COMMAND_POLICY_H

#include <stdint.h>

/*!
    @addtogroup MegaPolicy
    @{
*/

/*!
    @brief    Decide se un comando `water` può essere accettato.

    @details  L'irrigazione è consentita solo se è trascorso almeno
              `minIntervalMs` dall'ultima accettata: anti over-watering e
              anti-flood, in aggiunta al cap di durata (60 s) e al rifiuto se
              la pompa è già attiva (gestiti altrove). L'aritmetica è
              `uint32_t` non firmata, quindi è wraparound-safe rispetto al
              rollover di `millis()` (~49 giorni).

    @param[in] nowMs          Timestamp corrente (`millis()`).
    @param[in] lastAcceptedMs Timestamp dell'ultima irrigazione accettata;
                               `0` indica "nessuna ancora accettata".
    @param[in] minIntervalMs  Intervallo minimo richiesto tra due irrigazioni, in ms.

    @return   `true` se il comando può essere eseguito, `false` se va rifiutato
              per rate-limit.

    @note     Se `lastAcceptedMs == 0` la prima irrigazione è sempre consentita.
*/
static inline bool waterAllowed(uint32_t nowMs, uint32_t lastAcceptedMs,
                                uint32_t minIntervalMs) {
    if (lastAcceptedMs == 0) return true;
    return (nowMs - lastAcceptedMs) >= minIntervalMs;
}

/*!
    @brief    Determina se i parametri di movimento sono effettivamente cambiati.

    @details  Usata prima di una `setMotionParams` per evitare scritture EEPROM
              quando il valore richiesto è identico a quello già persistito
              (anti-usura della cella, l'EEPROM ha un numero finito di cicli
              di scrittura).

    @param[in] curRev  Durata retromarcia di avoidance attualmente salvata (ms).
    @param[in] curTurn Durata rotazione di avoidance attualmente salvata (ms).
    @param[in] newRev  Nuova durata retromarcia richiesta (ms).
    @param[in] newTurn Nuova durata rotazione richiesta (ms).

    @return   `true` se almeno uno dei due parametri differisce dal valore corrente.
*/
static inline bool motionParamsChanged(uint16_t curRev, uint16_t curTurn,
                                       uint16_t newRev, uint16_t newTurn) {
    return (curRev != newRev) || (curTurn != newTurn);
}

/*!
    @brief    Limita la durata richiesta per l'attivazione della pompa.

    @details  Defense-in-depth: l'Hub limita già la durata lato suo, ma il Mega
              riapplica comunque il clamp per proteggere pianta e pompa da
              comandi malformati o da un Hub compromesso.

    @param[in] ms    Durata richiesta per l'irrigazione, in ms.
    @param[in] maxMs Durata massima di sicurezza consentita, in ms.

    @return   `ms` se entro il limite, altrimenti `maxMs`.
*/
static inline uint32_t clampWaterDurationMs(uint32_t ms, uint32_t maxMs) {
    return (ms > maxMs) ? maxMs : ms;
}

/*!
    @brief    Limita un parametro di movimento (reverse/turn) a un range sicuro.

    @param[in] ms Valore richiesto, in ms.
    @param[in] lo Limite inferiore consentito, in ms.
    @param[in] hi Limite superiore consentito, in ms.

    @return   `ms` clampato nell'intervallo `[lo, hi]`.
*/
static inline uint16_t clampMotionParamMs(uint32_t ms, uint16_t lo, uint16_t hi) {
    if (ms < (uint32_t)lo) return lo;
    if (ms > (uint32_t)hi) return hi;
    return (uint16_t)ms;
}

/*! @} */ // MegaPolicy

#endif // COMMAND_POLICY_H
