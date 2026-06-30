#ifndef COMMAND_POLICY_H
#define COMMAND_POLICY_H

#include <stdint.h>

// =====================================================================
// Politiche di comando PURE (nessuna dipendenza HW) → unit-testabili a host.
// Tenute qui per poterle includere sia dal firmware (Communication.cpp) sia
// dai test nativi (tests/host/) senza tirare dentro Arduino.
// =====================================================================

// Irrigazione consentita solo se e' passato >= minIntervalMs dall'ultima
// accettata. lastAcceptedMs == 0 => prima irrigazione, sempre consentita.
// Anti over-watering + anti-flood (in aggiunta al cap 60 s e al rifiuto se la
// pompa e' gia' attiva). Aritmetica unsigned: wraparound-safe di millis().
static inline bool waterAllowed(uint32_t nowMs, uint32_t lastAcceptedMs,
                                uint32_t minIntervalMs) {
    if (lastAcceptedMs == 0) return true;
    return (nowMs - lastAcceptedMs) >= minIntervalMs;
}

// I motion params vanno riscritti in EEPROM solo se cambiano (anti-usura cella).
static inline bool motionParamsChanged(uint16_t curRev, uint16_t curTurn,
                                       uint16_t newRev, uint16_t newTurn) {
    return (curRev != newRev) || (curTurn != newTurn);
}

// Defense-in-depth (anche se l'Hub gia' limita): clamp della durata pompa a un
// massimo di sicurezza, per proteggere pianta/pompa da comandi malformati anche
// se l'Hub fosse compromesso.
static inline uint32_t clampWaterDurationMs(uint32_t ms, uint32_t maxMs) {
    return (ms > maxMs) ? maxMs : ms;
}

// Clamp dei parametri di movimento (reverse/turn di avoidance) a un range sicuro.
static inline uint16_t clampMotionParamMs(uint32_t ms, uint16_t lo, uint16_t hi) {
    if (ms < (uint32_t)lo) return lo;
    if (ms > (uint32_t)hi) return hi;
    return (uint16_t)ms;
}

#endif // COMMAND_POLICY_H
