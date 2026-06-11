#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <Arduino.h>

// Driver minimale per HC-SR04.
//
// Sostituisce la libreria enjoyneering/HCSR04, che inserisce un delay(50)
// bloccante dentro ogni getDistance(): con 6 sonde in round-robin sono
// ~300 ms di stallo del main loop per ciclo completo. Qui l'unica attesa
// e' il pulseIn con timeout proporzionale a maxDistanceCm, cosi' il costo
// di una lettura resta sotto ~12 ms per sonde con portata 200 cm.
//
// Vincolo d'uso: non rileggere la stessa sonda prima di ~60 ms (l'eco
// residuo deve estinguersi). Il round-robin a 6 sonde con un campione
// ogni 30 ms rispetta il vincolo con ampio margine (180 ms per sonda).
class Ultrasonic {
public:
    Ultrasonic(uint8_t triggerPin, uint8_t echoPin, uint16_t maxDistanceCm = 200);

    void begin();

    // Distanza in cm, NAN se nessun eco entro il timeout
    // (fuori portata o sensore scollegato).
    float readCm();

private:
    uint8_t       _triggerPin;
    uint8_t       _echoPin;
    unsigned long _timeoutUs;
};

#endif // ULTRASONIC_H
