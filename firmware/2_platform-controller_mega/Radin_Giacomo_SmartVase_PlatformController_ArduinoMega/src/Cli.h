/*!
    @file   Cli.h

    @ingroup MegaCli

    @brief  CLI seriale di debug del Mega, sulla porta USB.

    @details Usa `Serial` (115200 baud, terminatore `\n`) e non interferisce
             con la seriale verso l'Hub (`Serial1`, vedi `Communication`).
             Comandi disponibili:

             | Comando                    | Effetto |
             |-----------------------------|---------|
             | `help` / `?`                | Lista comandi |
             | `version`                   | Versione firmware |
             | `status`                    | Modalità operativa + stato runtime + RAM |
             | `stats`                     | Statistiche cumulative dalla EEPROM |
             | `config`                    | Configurazione corrente |
             | `sensors`                   | Ultime letture sensori |
             | `diag`                      | Diagnostica guidata: per ogni sensore/motore, stato + cosa controllare se non funziona |
             | `tank`                      | Stato tanica (livello, soglia, verdetto) |
             | `tank <cm>`                 | Imposta soglia tanica-vuota (persistita) |
             | `light <adc>`               | Imposta soglia luminosità 0..1023 (persistita): usata sia dal seeking LIGHT/SHADOW sia dalle luci UVA |
             | `rtc`                       | Epoch corrente + validità oscillatore (chip reale o clock software) |
             | `rtc set <epoch>`           | Imposta l'ora (epoch Unix in s); se il chip DS3232 non risponde, attiva un clock software di fallback (`millis()`-based, si perde al reset) |
             | `mode <idle\|light\|shadow>` | Cambio modalità operativa |
             | `motor <f\|b\|l\|r> <ms>`    | Test motori (max 60000 ms, ruote sollevate) |
             | `motortest`                 | Sequenza guidata f/b/l/r per verificare versi/mapping |
             | `calib <left> <right>`      | Calibrazione PWM motori 0..255 (persistita) |
             | `pump <ms>`                 | Test pompa (max 60000 ms, blocco se tanica vuota) |
             | `standalone <on\|off>`       | Sospende il deadman Hub per test a banco |
             | `reboot`                    | Soft reset via WDT |

    @date   2026-05-20

    @author Giacomo Radin
*/

#ifndef CLI_H
#define CLI_H

#include <Arduino.h>

class Movement;
class Sensors;
class Pump;
class GrowLight;
class Persistence;
struct SystemStatus;

/*!
    @addtogroup MegaCli
    @{
*/

/*!
    @class  Cli
    @brief  Parser e dispatcher dei comandi CLI seriali su USB (vedi tabella comandi in Cli.h).
*/
class Cli {
public:
    /*! @brief Costruttore: inizializza il buffer di linea vuoto. */
    Cli();

    /*!
        @brief    Legge i caratteri disponibili da `Serial` senza bloccare e, a fine
                   linea (`\n`), esegue il comando corrispondente.
        @details  Va chiamato ad ogni iterazione del main loop. Accumula i caratteri
                   in un buffer fisso (`BUF_SIZE`) finché non riceve il terminatore.
        @param[in,out] mv  Stato/comandi del modulo Movement.
        @param[in,out] sn  Stato/letture del modulo Sensors.
        @param[in,out] pp  Stato/comandi del modulo Pump.
        @param[in,out] gl  Stato del relè luci di coltivazione (GrowLight).
        @param[in,out] ps  Configurazione/statistiche persistite (Persistence).
        @param[in,out] sys Stato di sistema condiviso (SystemStatus).
    */
    void tick(Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl,
              Persistence& ps, SystemStatus& sys);

private:
    /*! @brief Effettua il parsing di una linea di comando completa e ne esegue l'azione. */
    void execute(const char* line, Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl,
                 Persistence& ps, SystemStatus& sys);
    /*! @brief Prints the list of available commands (`help`/`?` command). */
    void printHelp();
    /*! @brief Prints operating mode, runtime state and free RAM (`status` command). */
    void printStatus(Movement& mv, Pump& pp, GrowLight& gl, SystemStatus& sys);
    /*! @brief Prints the cumulative statistics read from EEPROM (`stats` command). */
    void printStats(Persistence& ps);
    /*! @brief Prints the current configuration read from EEPROM (`config` command). */
    void printConfig(Persistence& ps);
    /*! @brief Prints the latest sensor readings (`sensors` command). */
    void printSensors(Sensors& sn);
    /*! @brief Runs the guided sensor/motor diagnostics, with troubleshooting hints (`diag` command). */
    void printDiag(Sensors& sn, Movement& mv, Pump& pp, GrowLight& gl, Persistence& ps, SystemStatus& sys);
    /*! @brief Prints the tank status (level, threshold, empty/full verdict) (`tank` command). */
    void printTank(Sensors& sn, Pump& pp, Persistence& ps);

    static const size_t BUF_SIZE = 64;  /**< Size of the CLI line accumulation buffer. */
    char  buf[BUF_SIZE];                /**< Buffer of received characters, waiting for the terminator. */
    size_t pos;                         /**< Current write position in buf. */
};

/*! @} */ // MegaCli

#endif // CLI_H
