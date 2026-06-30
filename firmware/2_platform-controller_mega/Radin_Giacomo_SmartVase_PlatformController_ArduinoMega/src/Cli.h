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
             | `rtc`                       | Epoch corrente + validità oscillatore |
             | `rtc set <epoch>`           | Imposta l'ora del DS3232 (epoch Unix in s) |
             | `mode <idle\|light\|shadow>` | Cambio modalità operativa |
             | `motor <f\|b\|l\|r> <ms>`    | Test motori (max 5000 ms) |
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
        @param[in,out] ps  Configurazione/statistiche persistite (Persistence).
        @param[in,out] sys Stato di sistema condiviso (SystemStatus).
    */
    void tick(Movement& mv, Sensors& sn, Pump& pp,
              Persistence& ps, SystemStatus& sys);

private:
    /*! @brief Effettua il parsing di una linea di comando completa e ne esegue l'azione. */
    void execute(const char* line, Movement& mv, Sensors& sn, Pump& pp,
                 Persistence& ps, SystemStatus& sys);
    /*! @brief Stampa l'elenco dei comandi disponibili (comando `help`/`?`). */
    void printHelp();
    /*! @brief Stampa modalità operativa, stato runtime e RAM libera (comando `status`). */
    void printStatus(Movement& mv, Pump& pp, SystemStatus& sys);
    /*! @brief Stampa le statistiche cumulative lette dalla EEPROM (comando `stats`). */
    void printStats(Persistence& ps);
    /*! @brief Stampa la configurazione corrente letta dalla EEPROM (comando `config`). */
    void printConfig(Persistence& ps);
    /*! @brief Stampa le ultime letture dei sensori (comando `sensors`). */
    void printSensors(Sensors& sn);
    /*! @brief Esegue la diagnostica guidata sensori/motori, con suggerimenti in caso di anomalia (comando `diag`). */
    void printDiag(Sensors& sn, Movement& mv, Pump& pp, Persistence& ps, SystemStatus& sys);
    /*! @brief Stampa lo stato della tanica (livello, soglia, verdetto vuota/piena) (comando `tank`). */
    void printTank(Sensors& sn, Pump& pp, Persistence& ps);

    static const size_t BUF_SIZE = 64;  /**< Dimensione del buffer di accumulo linea CLI. */
    char  buf[BUF_SIZE];                /**< Buffer dei caratteri ricevuti, in attesa del terminatore. */
    size_t pos;                         /**< Posizione corrente di scrittura in buf. */
};

/*! @} */ // MegaCli

#endif // CLI_H
