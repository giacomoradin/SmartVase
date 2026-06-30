/*!
    @file   SensorPolicy.h

    @ingroup MegaSensors

    @brief  Decisioni pure derivate dalle letture dei sensori.

    @details Funzioni `static inline` senza dipendenze hardware, condivise tra
             il firmware (`Sensors.cpp`, `Movement.cpp`) e i test unitari a
             host in `tests/host/` (vedi `test_sensor_policy`). Incapsulano
             le regole di sicurezza/comportamento derivate da letture
             analogiche o ad ultrasuoni, separate dall'accesso HW vero e
             proprio.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef SENSOR_POLICY_H
#define SENSOR_POLICY_H

#include <stdint.h>
#include <math.h>   // isnan, NAN

/*!
    @addtogroup MegaSensors
    @{
*/

/*!
    @brief    Determina se la tanica dell'acqua va considerata vuota.

    @details  Fail-safe: se la lettura del sensore US4 (sulla tanica) non è
              valida (`NaN`, es. timeout dell'ultrasuono) si considera la
              tanica vuota e si blocca la pompa, perché senza una misura
              affidabile non si può autorizzare l'irrigazione. US4 guarda
              l'acqua dall'alto, quindi una distanza maggiore corrisponde a un
              livello d'acqua più basso: oltre `thresholdCm` la tanica è vuota.

    @param[in] waterLevelCm Distanza US4→pelo dell'acqua, in cm (`NAN` se non valida).
    @param[in] thresholdCm  Soglia oltre la quale la tanica è considerata vuota, in cm.

    @return   `true` se la tanica è vuota (o la lettura non è affidabile), `false` altrimenti.
*/
static inline bool tankConsideredEmpty(float waterLevelCm, uint16_t thresholdCm) {
    return isnan(waterLevelCm) || waterLevelCm > (float)thresholdCm;
}

/*!
    @brief    Determina se il robot deve sterzare durante il seeking luce/ombra.

    @details  In modalità `LIGHT` il robot cerca la luce: se `lux` è sotto la
              soglia (troppo buio) deve sterzare. In modalità `SHADOW` cerca
              l'ombra: se `lux` è sopra la soglia (troppa luce) deve sterzare.
              `lux < 0` indica che la lettura ADC del fotoresistore non è
              ancora valida, nel qual caso non si sterza (fail-safe: si va
              dritti finché non si ha una lettura affidabile).

    @param[in] seekingLight  `true` se la modalità target corrente è `LIGHT`.
    @param[in] seekingShadow `true` se la modalità target corrente è `SHADOW`.
    @param[in] lux           Valore ADC di luminosità corrente (negativo = non valido).
    @param[in] threshold     Soglia ADC di luminosità configurata (`light_threshold`).

    @return   `true` se il robot deve sterzare verso la condizione cercata.
*/
static inline bool seekWantsTurn(bool seekingLight, bool seekingShadow,
                                 int lux, int threshold) {
    if (lux < 0) return false;
    if (seekingLight)  return lux < threshold;
    if (seekingShadow) return lux > threshold;
    return false;
}

/*! @} */ // MegaSensors

#endif // SENSOR_POLICY_H
