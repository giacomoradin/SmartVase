/*!
    @file   Persistence.h

    @ingroup MegaPersistence

    @brief  Persistenza EEPROM dual-slot (wear-leveling) di config e statistiche.

    @date   2026-04-29

    @author Giacomo Radin
*/

#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <Arduino.h>
#include <EEPROM.h>
#include "smartvase_aliases.h"

/*!
    @addtogroup MegaPersistence
    @{
*/

/*!
    @class Persistence
    @brief Gestore della persistenza dei dati di configurazione e statistiche su EEPROM per Arduino Mega.

    @details Implementa un meccanismo a doppio slot per prevenire corruzioni di dati in caso di spegnimento
             durante la scrittura (wear leveling e fail-safe), protetto da CRC16 (Crc16.h) e magic number.
             Al load sceglie lo slot valido col `write_counter` più alto; al save scrive sempre sullo slot
             opposto a quello corrente. Implementa inoltre un throttling temporale delle scritture
             (`saveConfig`/`saveStats`, vedi le costanti `EEPROM_*_WRITE_INTERVAL` in Persistence.cpp) per
             non usurare prematuramente la EEPROM, con possibilità di forzare la scrittura immediata.
*/
class Persistence {
public:
    /**
     * @brief Costruttore della classe Persistence.
     */
    Persistence();

    /**
     * @brief Carica la configurazione del dispositivo (soglie, modalità, ecc.) dalla EEPROM.
     * 
     * Legge entrambi gli slot, convalida tramite CRC16 e sceglie lo slot con contatore di scrittura più alto.
     */
    void loadConfig();

    /**
     * @brief Salva la configurazione corrente in EEPROM.
     * 
     * Scrive nello slot alternativo rispetto all'ultimo caricato per distribuire l'usura.
     * 
     * @param force Se impostato a true, ignora il throttling temporale (default: false).
     */
    void saveConfig(bool force = false);

    /**
     * @brief Carica le statistiche cumulative del dispositivo dalla EEPROM.
     */
    void loadStats();

    /**
     * @brief Salva le statistiche cumulative correnti in EEPROM.
     * 
     * @param force Se impostato a true, ignora il throttling temporale (default: false).
     */
    void saveStats(bool force = false);

    /**
     * @brief Restituisce il riferimento alla configurazione corrente.
     * @return DeviceConfig& Configurazione del dispositivo.
     */
    DeviceConfig&    getConfig() { return config; }

    /**
     * @brief Restituisce il riferimento alle statistiche cumulative correnti.
     * @return CumulativeStats& Statistiche di utilizzo.
     */
    CumulativeStats& getStats()  { return stats; }

private:
    DeviceConfig    config;             /**< Struttura dati per le configurazioni correnti */
    CumulativeStats stats;              /**< Struttura dati per le statistiche correnti */
    CumulativeStats lastSavedStats;     /**< Copia dell'ultimo salvataggio per verificare modifiche reali */
    unsigned long   lastEepromConfigWrite; /**< Timestamp (in ms) dell'ultima scrittura di configurazione */
    unsigned long   lastEepromStatsWrite;  /**< Timestamp (in ms) dell'ultima scrittura di statistiche */
    uint8_t         current_config_slot;   /**< Slot di configurazione attivo (0 o 1) */
    uint8_t         current_stats_slot;    /**< Slot di statistiche attivo (0 o 1) */
};

/*! @} */ // MegaPersistence

#endif // PERSISTENCE_H
