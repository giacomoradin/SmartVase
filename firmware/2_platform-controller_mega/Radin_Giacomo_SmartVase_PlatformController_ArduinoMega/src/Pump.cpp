/*!
    @file   Pump.cpp

    @ingroup MegaPump

    @brief  Implementazione del driver relè pompa (vedi Pump.h).

    @date   2026-05-20

    @author Giacomo Radin
*/

#include "Pump.h"

// PIN map autoritativo: relay IN1 = D10 (pompa), IN2 = D11 (riservato).
#define PUMP_RELAY_PIN          10
#define PUMP_RELAY_BACKUP_PIN   11

// I moduli rele' tipici a 5V su Arduino sono attivi BASSI: GPIO LOW = relay ON.
// Cambiare a 0 se il modulo in uso e' attivo alto.
#define PUMP_RELAY_ACTIVE_LOW   1

// Durata massima ammessa per una singola sessione di irrigazione (safety).
// Sopra questa soglia il comando viene rifiutato.
#define PUMP_MAX_DURATION_MS    60000UL  // 60 s

/*! @brief Attiva il relè della pompa (gestisce la polarità via `PUMP_RELAY_ACTIVE_LOW`). */
static inline void pumpOn() {
#if PUMP_RELAY_ACTIVE_LOW
    digitalWrite(PUMP_RELAY_PIN, LOW);
#else
    digitalWrite(PUMP_RELAY_PIN, HIGH);
#endif
}

/*! @brief Disattiva il relè della pompa (gestisce la polarità via `PUMP_RELAY_ACTIVE_LOW`). */
static inline void pumpOff() {
#if PUMP_RELAY_ACTIVE_LOW
    digitalWrite(PUMP_RELAY_PIN, HIGH);
#else
    digitalWrite(PUMP_RELAY_PIN, LOW);
#endif
}

Pump::Pump() : active(false), start_ms(0), duration_ms_target(0) {}

void Pump::init() {
    // Prima il livello di riposo, poi pinMode: su AVR scrivere il PORT con il
    // pin ancora INPUT pre-carica il latch, evitando il glitch LOW (= pompa
    // accesa per qualche microsecondo con rele' attivo-basso) al passaggio
    // a OUTPUT.
    pumpOff();
#if PUMP_RELAY_ACTIVE_LOW
    digitalWrite(PUMP_RELAY_BACKUP_PIN, HIGH);
#else
    digitalWrite(PUMP_RELAY_BACKUP_PIN, LOW);
#endif
    pinMode(PUMP_RELAY_PIN, OUTPUT);
    pinMode(PUMP_RELAY_BACKUP_PIN, OUTPUT);
}

bool Pump::start(uint32_t duration_ms, CumulativeStats& stats) {
    if (active) return false;
    if (duration_ms == 0 || duration_ms > PUMP_MAX_DURATION_MS) return false;
    pumpOn();
    active             = true;
    start_ms           = millis();
    duration_ms_target = duration_ms;
    stats.total_irrigations++;
    return true;
}

void Pump::stop(CumulativeStats& stats) {
    if (!active) {
        pumpOff();
        return;
    }
    pumpOff();
    uint32_t elapsed = (uint32_t)(millis() - start_ms);
    stats.total_irrigation_duration_s += (elapsed + 500) / 1000UL;
    active             = false;
    duration_ms_target = 0;
}

void Pump::tick(CumulativeStats& stats) {
    if (!active) return;
    if ((uint32_t)(millis() - start_ms) >= duration_ms_target) {
        stop(stats);
    }
}
