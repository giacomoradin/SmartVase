#ifndef SENSOR_POLICY_H
#define SENSOR_POLICY_H

#include <stdint.h>
#include <math.h>   // isnan, NAN

// =====================================================================
// Decisioni PURE derivate dai sensori (nessuna dipendenza HW) → unit-testabili
// a host (tests/host/). Incluse sia dal firmware (Sensors/Movement) sia dai test.
// =====================================================================

// Tanica considerata vuota se la lettura US4 non e' valida (NaN → fail-safe:
// senza una misura affidabile non si pompa) oppure se la distanza acqua supera
// la soglia (US4 guarda l'acqua dall'alto: piu' lontano = livello piu' basso).
static inline bool tankConsideredEmpty(float waterLevelCm, uint16_t thresholdCm) {
    return isnan(waterLevelCm) || waterLevelCm > (float)thresholdCm;
}

// Il robot deve sterzare verso la sorgente in seeking? lux < 0 = lettura ADC non
// ancora valida (nessuna sterzata). LIGHT: troppo buio (lux < soglia) → cerca
// luce. SHADOW: troppa luce (lux > soglia) → cerca ombra.
static inline bool seekWantsTurn(bool seekingLight, bool seekingShadow,
                                 int lux, int threshold) {
    if (lux < 0) return false;
    if (seekingLight)  return lux < threshold;
    if (seekingShadow) return lux > threshold;
    return false;
}

#endif // SENSOR_POLICY_H
