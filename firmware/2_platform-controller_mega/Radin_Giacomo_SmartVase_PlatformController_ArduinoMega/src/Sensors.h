/*!
 * @file Sensors.h
 * @ingroup MegaSensors
 * @brief Modulo unificato di lettura sensori del Platform Controller (ultrasuoni, ADC, RTC, BME680).
 * @date 2026-04-29
 * @author Giacomo Radin
 */

/**
 * @defgroup MegaSensors Sensori e RTC (Mega)
 * @brief Lettura sensori ultrasuoni round-robin con EMA, ADC suolo/luce/batteria, BME680 opzionale, RTC DS3232, policy pure di interpretazione sensori.
 * @{
 */

#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "Ultrasonic.h"
#include "RtcDs3232.h"
#include "smartvase_aliases.h"
#include "SensorPolicy.h"

// --- Feature flags ---
/**
 * @def BME680_ENABLED
 * @brief Flag di abilitazione per il sensore di temperatura/pressione/umidità/gas BME680.
 * 
 * Il BME680 non è montato sul prototipo attuale (PIN map 2026-05-19, conferma 2026-06-11).
 * Tenere a 0 finché non viene cablato. Se impostato a 1, riattiva probe I2C, letture in TelemetryDeep e log.
 */
#define BME680_ENABLED 0

/**
 * @def BATTERY_MONITORING_ENABLED
 * @brief Flag di abilitazione per il monitoraggio della tensione di batteria.
 * 
 * La batteria non è ancora cablata sul nuovo prototipo. Se impostato a 1, riattiva le letture del partitore.
 */
#define BATTERY_MONITORING_ENABLED 0

#if BME680_ENABLED
#include <Adafruit_BME680.h>
#endif

/**
 * @class Sensors
 * @brief Modulo per la gestione unificata dei sensori di SmartVase (ultrasuoni, fotocellula, igrometro, RTC, BME680).
 *
 * Esegue le letture dei 6 sensori ad ultrasuoni HC-SR04 in modalità round-robin non bloccante (una lettura ogni
 * `US_SAMPLE_INTERVAL_MS`, vedi Sensors.cpp), applica filtri EMA (Exponential Moving Average) per stabilizzare le
 * letture rumorose, e fornisce dati filtrati ed elaborati sia in locale (per Movement, comandi CLI/seriali) sia
 * per la costruzione dei pacchetti di telemetria Protobuf (`TelemetryFast`/`TelemetryDeep`).
 *
 * @note Nessuna chiamata bloccante nel polling: `sampleSensors()` va richiamata ad alta frequenza dal main loop
 *       non bloccante (niente `delay()`), coerentemente con le convenzioni del firmware Mega.
 */
class Sensors {
public:
    /**
     * @brief Costruttore della classe Sensors.
     */
    Sensors();

    /**
     * @brief Inizializza i pin dei sensori e i bus di comunicazione (I2C/Wire per RTC e BME).
     */
    void init();

    /**
     * @brief Esegue il polling non bloccante round-robin dei 6 sensori ad ultrasuoni e degli input analogici.
     * 
     * Da chiamare ad alta frequenza all'interno del main loop.
     */
    void sampleSensors();

    // --- Letture filtrate (cm) ---
    /** @brief Restituisce la distanza dal sensore US1 (Frontale Alto) in cm. */
    float getTopDist()        const { return cached_top_dist_cm; }
    /** @brief Restituisce la distanza dal sensore US2 (Frontale Destro) in cm. */
    float getFrontRightDist() const { return cached_front_right_dist_cm; }
    /** @brief Restituisce la distanza dal sensore US3 (Frontale Sinistro) in cm. */
    float getFrontLeftDist()  const { return cached_front_left_dist_cm; }
    /** @brief Restituisce la lettura del livello dell'acqua della tanica US4 in cm. */
    float getWaterLevel()     const { return cached_water_level_cm; }
    /** @brief Restituisce la distanza dal sensore US5 (Laterale Sinistro) in cm. */
    float getLeftDist()       const { return cached_left_dist_cm; }
    /** @brief Restituisce la distanza dal sensore US6 (Laterale Destro) in cm. */
    float getRightDist()      const { return cached_right_dist_cm; }

    /**
     * @brief Verifica se la tanica dell'acqua è vuota.
     * 
     * Esegue un controllo fail-safe: se la lettura di US4 non è valida o supera la soglia di serbatoio vuoto.
     * 
     * @param thresholdCm Soglia limite di livello acqua in cm (distanza sensore-acqua).
     * @return true se la tanica è considerata vuota o la lettura non è valida.
     */
    bool tankLooksEmpty(uint16_t thresholdCm) const {
        return tankConsideredEmpty(cached_water_level_cm, thresholdCm);
    }

    // --- ADC ---
    /** @brief Restituisce il valore letto dall'LDR (luminosità, 0-1023). */
    int getLux()           const { return cached_lux; }
    /** @brief Restituisce il valore di umidità del terreno dall'igrometro (0-1023). */
    int getSoilMoisture()  const { return cached_soil_moisture; }
    /** @brief Restituisce la tensione stimata della batteria in Volt. */
    float getBatteryVoltage() const { return cached_battery_voltage; }

    // --- RTC ---
    /** @brief Legge e restituisce l'epoca UNIX corrente dall'RTC DS3232. */
    uint32_t getEpoch();
    /**
     * @brief Imposta l'epoca UNIX corrente dell'RTC.
     * @param epoch_s Timestamp UNIX da impostare (in secondi).
     * @return true se l'operazione ha avuto successo.
     */
    bool setEpoch(uint32_t epoch_s);
    /** @brief Restituisce lo stato di funzionamento dell'RTC. */
    bool getRTCStatus() const { return rtc_status; }
    /** @brief Verifica se l'oscillatore dell'RTC si è fermato (es. batteria tampone scarica). */
    bool rtcOscStopped() { return rtc_status ? rtc.oscillatorStopped() : true; }

    // --- BME680 ---
    /** @brief Restituisce lo stato di funzionamento/rilevamento del sensore BME680. */
    bool getBMEStatus() const { return bme_status; }

    /**
     * @brief Costruisce il messaggio Protobuf TelemetryFast.
     * 
     * @param movState Stato corrente della state machine dei motori.
     * @param deviceId ID del dispositivo Mega.
     * @return TelemetryFast Struttura compilata con le telemetrie rapide.
     */
    TelemetryFast buildFastTelemetry(CppMovementState movState, const char* deviceId);

    /**
     * @brief Costruisce il messaggio Protobuf TelemetryDeep.
     * 
     * @param stats Statistiche cumulative da inviare al cloud.
     * @param deviceId ID del dispositivo Mega.
     * @return TelemetryDeep Struttura compilata con le telemetrie profonde.
     */
    TelemetryDeep buildDeepTelemetry(CumulativeStats& stats, const char* deviceId);

private:
    /**
     * @brief Applica un filtro Exponential Moving Average (EMA) ad una lettura grezza, con scarto dei valori
     *        fuori range e fallback a NAN dopo letture invalide consecutive.
     *
     * @param[in] raw_value     Ultima lettura grezza dal sensore (puo' essere NAN se il sensore ha avuto timeout).
     * @param[in] last_value    Valore filtrato precedente (stato del filtro), usato come base della media mobile.
     * @param[in,out] invalid_streak Contatore di letture invalide consecutive; azzerato alla prima lettura valida,
     *                          incrementato altrimenti. Oltre una soglia interna il filtro "dimentica" lo stato
     *                          e restituisce NAN (vedi Sensors.cpp per il valore esatto della soglia).
     * @param[in] min_valid     Limite inferiore del range plausibile per la lettura (cm).
     * @param[in] max_valid     Limite superiore del range plausibile per la lettura (cm), tipicamente la portata
     *                          massima configurata per la sonda HC-SR04.
     * @return Valore filtrato (EMA) oppure NAN se il sensore e' considerato non affidabile.
     */
    float applyEmaFilter(float raw_value, float last_value, unsigned int& invalid_streak,
                         float min_valid, float max_valid);
    /**
     * @brief Esegue il trigger e la lettura del successivo sensore ad ultrasuoni nella sequenza round-robin.
     * @note Avanza internamente l'indice di ciclo (`us_cycle_idx`) sulle 6 sonde; aggiorna la relativa cache
     *       filtrata (EMA) e il relativo contatore di letture invalide consecutive.
     */
    void  sampleNextUltrasonic();
    /**
     * @brief Campiona i canali analogici dell'ADC (lux, igrometro, tensione di batteria se abilitata).
     * @note Applica un filtro EMA leggero su lux e umidita' suolo per stabilizzare letture rumorose; la lettura
     *       della batteria e' condizionata al flag #BATTERY_MONITORING_ENABLED.
     */
    void  sampleAdcChannels();

#if BME680_ENABLED
    Adafruit_BME680 bme; /**< Driver del sensore ambientale BME680 (attivo solo se #BME680_ENABLED == 1). */
#endif
    RtcDs3232 rtc; /**< Driver locale dell'RTC DS3232 (I2C 0x68). */

    Ultrasonic us1_top;          /**< Sensore ultrasuoni Frontale Alto (US1) */
    Ultrasonic us2_front_right;  /**< Sensore ultrasuoni Frontale Destro (US2) */
    Ultrasonic us3_front_left;   /**< Sensore ultrasuoni Frontale Sinistro (US3) */
    Ultrasonic us4_water;        /**< Sensore ultrasuoni Livello Acqua Tanica (US4) */
    Ultrasonic us5_left;         /**< Sensore ultrasuoni Laterale Sinistro (US5) */
    Ultrasonic us6_right;        /**< Sensore ultrasuoni Laterale Destro (US6) */

    uint8_t us_cycle_idx;        /**< Indice per la gestione del polling sequenziale (0..5) */
    unsigned long last_us_sample_ms;/**< Timestamp dell'ultimo campionamento di un sensore ad ultrasuoni */

    float cached_top_dist_cm;          /**< Ultima distanza filtrata (EMA) di US1, in cm; NAN se non valida. */
    float cached_front_right_dist_cm;  /**< Ultima distanza filtrata (EMA) di US2, in cm; NAN se non valida. */
    float cached_front_left_dist_cm;   /**< Ultima distanza filtrata (EMA) di US3, in cm; NAN se non valida. */
    float cached_water_level_cm;       /**< Ultima distanza filtrata (EMA) di US4 (tanica), in cm; NAN se non valida. */
    float cached_left_dist_cm;         /**< Ultima distanza filtrata (EMA) di US5, in cm; NAN se non valida. */
    float cached_right_dist_cm;        /**< Ultima distanza filtrata (EMA) di US6, in cm; NAN se non valida. */

    unsigned int invalid_streak_top;    /**< Letture consecutive fuori range/invalide per US1. */
    unsigned int invalid_streak_fr;     /**< Letture consecutive fuori range/invalide per US2. */
    unsigned int invalid_streak_fl;     /**< Letture consecutive fuori range/invalide per US3. */
    unsigned int invalid_streak_water;  /**< Letture consecutive fuori range/invalide per US4. */
    unsigned int invalid_streak_left;   /**< Letture consecutive fuori range/invalide per US5. */
    unsigned int invalid_streak_right;  /**< Letture consecutive fuori range/invalide per US6. */

    int   cached_lux;             /**< Ultimo valore ADC filtrato dell'LDR (0-1023); -1 se non ancora campionato. */
    int   cached_soil_moisture;   /**< Ultimo valore ADC filtrato della forcella umidita' suolo (0-1023); -1 se non ancora campionato. */
    float cached_battery_voltage; /**< Ultima tensione di batteria stimata (V); NAN se monitoraggio disabilitato o non letta. */

    bool bme_status; /**< true se il BME680 e' stato rilevato e inizializzato correttamente in init(). */
    bool rtc_status;  /**< true se l'RTC DS3232 ha risposto su I2C in init(). */
};

#endif // SENSORS_H

/** @} */ // end of MegaSensors
