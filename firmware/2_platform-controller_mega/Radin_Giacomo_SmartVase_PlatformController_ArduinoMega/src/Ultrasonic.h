/*!
 * @file Ultrasonic.h
 * @ingroup MegaSensors
 * @brief Driver locale non bloccante per sensori ad ultrasuoni HC-SR04.
 * @date 2026-06-11
 * @author Giacomo Radin
 */

#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <Arduino.h>

/**
 * @class Ultrasonic
 * @brief Driver minimale per una singola sonda HC-SR04 (trigger/echo), con timeout configurabile.
 *
 * Sostituisce la libreria enjoyneering/HCSR04, che inserisce un `delay(50)` bloccante dentro ogni
 * `getDistance()`: con 6 sonde in round-robin sarebbero ~300 ms di stallo del main loop per ciclo completo.
 * Qui l'unica attesa e' il `pulseIn` con timeout proporzionale a `maxDistanceCm`, cosi' il costo di una
 * lettura resta sotto ~12 ms per sonde con portata 200 cm.
 *
 * @note Vincolo d'uso: non rileggere la stessa sonda prima di ~60 ms (l'eco residuo deve estinguersi).
 *       Il round-robin a 6 sonde con un campione ogni 30 ms (vedi Sensors.cpp) rispetta il vincolo con
 *       ampio margine (180 ms per sonda).
 */
class Ultrasonic {
public:
    /**
     * @brief Costruisce il driver per una sonda HC-SR04, calcolando il timeout di lettura dalla portata massima.
     * @param[in] triggerPin   Pin digitale collegato al pin TRIG della sonda.
     * @param[in] echoPin      Pin digitale collegato al pin ECHO della sonda.
     * @param[in] maxDistanceCm Portata massima utile in cm; determina il timeout interno del `pulseIn`
     *                          (default 200 cm se non specificato).
     */
    Ultrasonic(uint8_t triggerPin, uint8_t echoPin, uint16_t maxDistanceCm = 200);

    /**
     * @brief Configura i pin GPIO (TRIG come OUTPUT a LOW, ECHO come INPUT).
     * @note Da chiamare una volta in fase di inizializzazione, dopo che i pin sono stati assegnati.
     */
    void begin();

    /**
     * @brief Esegue un ciclo trigger+misura e restituisce la distanza rilevata.
     * @return Distanza in cm, oppure NAN se nessun eco viene ricevuto entro il timeout
     *         (sonda fuori portata o scollegata).
     * @note Bloccante per al piu' `_timeoutUs` microsecondi (il tempo del `pulseIn`); il chiamante
     *       (Sensors::sampleNextUltrasonic) e' responsabile di non invocarla piu' spesso del vincolo
     *       di ri-lettura della sonda.
     */
    float readCm();

private:
    uint8_t       _triggerPin; /**< Pin digitale TRIG della sonda. */
    uint8_t       _echoPin;    /**< Pin digitale ECHO della sonda. */
    unsigned long _timeoutUs;  /**< Timeout del pulseIn in microsecondi, derivato da maxDistanceCm. */
};

#endif // ULTRASONIC_H
