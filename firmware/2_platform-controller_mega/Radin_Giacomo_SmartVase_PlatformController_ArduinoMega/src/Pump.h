/*!
    @file   Pump.h

    @ingroup MegaPump

    @brief  Controllo non bloccante del relè della pompa di irrigazione.

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef PUMP_H
#define PUMP_H

#include <Arduino.h>
#include "smartvase_aliases.h"

/*!
    @addtogroup MegaPump
    @{
*/

/*!
    @class Pump
    @brief Modulo non bloccante per il controllo del relè della pompa di irrigazione.

    @details Permette di avviare l'irrigazione per una durata programmata e spegne
             automaticamente la pompa allo scadere del tempo tramite polling nel main
             loop (`tick()`, niente `delay()`). Aggiorna le statistiche cumulative di
             irrigazione (conteggio e durata totale). La gestione della polarità del
             relè (attivo basso/alto) e il cap di sicurezza sulla durata sono
             nell'implementazione (vedi Pump.cpp).
*/
class Pump {
public:
    /**
     * @brief Costruttore della classe Pump.
     */
    Pump();

    /**
     * @brief Inizializza il pin del relè della pompa impostandolo come output spento.
     */
    void init();

    /**
     * @brief Avvia l'irrigazione della pianta per una durata specificata.
     * 
     * @param duration_ms Durata dell'irrigazione in millisecondi.
     * @param stats Riferimento alle statistiche cumulative per registrare l'attivazione.
     * @return true se l'irrigazione è stata avviata con successo, false se la pompa era già attiva.
     */
    bool start(uint32_t duration_ms, CumulativeStats& stats);

    /**
     * @brief Forza l'arresto immediato della pompa.
     * 
     * Da chiamare anche in caso di emergenza o degraded mode.
     * 
     * @param stats Riferimento alle statistiche cumulative per aggiornare i contatori.
     */
    void stop(CumulativeStats& stats);

    /**
     * @brief Gestisce il timer dell'irrigazione ed esegue l'arresto automatico allo scadere.
     * 
     * Da chiamare ad alta frequenza nel loop principale.
     * 
     * @param stats Riferimento alle statistiche cumulative.
     */
    void tick(CumulativeStats& stats);

    /**
     * @brief Ritorna se la pompa è attualmente attiva.
     * @return true se attiva, false altrimenti.
     */
    bool isActive() const { return active; }

private:
    bool          active;             /**< Stato di attività del relè */
    unsigned long start_ms;           /**< Timestamp (in ms) dell'avvio della pompa */
    uint32_t      duration_ms_target; /**< Durata target dell'irrigazione corrente (in ms) */
};

/*! @} */ // MegaPump

#endif // PUMP_H
