/*!
    @file   Pump.cpp

    @ingroup MegaPump

    @brief  Implementation of the pump relay driver (see Pump.h).

    @date   2026-05-20

    @author Giacomo Radin
*/

#include "Pump.h"

/*! @brief Pump relay pin (IN1 = D10). IN2 = D11 belongs to GrowLight (UVA lights) and must not be touched here. */
#define PUMP_RELAY_PIN          10

/*! @brief Relay module polarity: 1 = active low (GPIO LOW = pump ON), like typical 5V modules; 0 = active high. */
#define PUMP_RELAY_ACTIVE_LOW   1

/*! @brief Maximum duration of a single irrigation (ms): past this threshold the command is rejected (safety). */
#define PUMP_MAX_DURATION_MS    60000UL  // 60 s

/*! @brief Activates the pump relay (handles polarity via `PUMP_RELAY_ACTIVE_LOW`). */
static inline void pumpOn() {
#if PUMP_RELAY_ACTIVE_LOW
    digitalWrite(PUMP_RELAY_PIN, LOW);
#else
    digitalWrite(PUMP_RELAY_PIN, HIGH);
#endif
}

/*! @brief Deactivates the pump relay (handles polarity via `PUMP_RELAY_ACTIVE_LOW`). */
static inline void pumpOff() {
#if PUMP_RELAY_ACTIVE_LOW
    digitalWrite(PUMP_RELAY_PIN, HIGH);
#else
    digitalWrite(PUMP_RELAY_PIN, LOW);
#endif
}

Pump::Pump() : active(false), start_ms(0), duration_ms_target(0) {}

void Pump::init() {
    // Rest level first, then pinMode: on AVR, writing the PORT while the
    // pin is still INPUT pre-charges the latch, avoiding the LOW glitch (=
    // pump on for a few microseconds with an active-low relay) when switching
    // to OUTPUT.
    pumpOff();
    pinMode(PUMP_RELAY_PIN, OUTPUT);
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
