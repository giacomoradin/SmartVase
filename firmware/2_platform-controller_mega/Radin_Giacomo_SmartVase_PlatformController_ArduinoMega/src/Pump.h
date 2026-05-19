#ifndef PUMP_H
#define PUMP_H

#include <Arduino.h>
#include "smartvase_aliases.h"

// Modulo non-bloccante per pilotare il rele' della pompa di irrigazione.
// Avvia start(duration_ms), poi tick() lo ferma da solo allo scadere del timer.
// Aggiorna anche le statistiche cumulative.
class Pump {
public:
    Pump();
    void init();

    // Avvia una sessione di irrigazione. Ritorna false se la pompa e' gia' attiva
    // o se la durata e' fuori range valido.
    bool start(uint32_t duration_ms, CumulativeStats& stats);

    // Forza lo stop immediato. Da chiamare anche in degraded mode.
    void stop(CumulativeStats& stats);

    // Polling nel main loop: ferma la pompa quando il timer scade.
    void tick(CumulativeStats& stats);

    bool isActive() const { return active; }

private:
    bool          active;
    unsigned long start_ms;
    uint32_t      duration_ms_target;
};

#endif // PUMP_H
