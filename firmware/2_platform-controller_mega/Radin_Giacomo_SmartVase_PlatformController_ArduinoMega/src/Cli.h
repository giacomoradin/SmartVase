#ifndef CLI_H
#define CLI_H

#include <Arduino.h>

class Movement;
class Sensors;
class Pump;
class Persistence;
struct SystemStatus;

// CLI seriale di debug sulla porta USB (Serial, 115200, terminatore '\n').
// Non interferisce con la seriale Hub<->Mega (Serial1).
//
//   help / ?                  lista comandi
//   version                   versione firmware
//   status                    modalita' operativa + stato runtime + RAM
//   stats                     statistiche cumulative dalla EEPROM
//   config                    configurazione corrente
//   sensors                   ultime letture sensori
//   diag                      diagnostica guidata: per ogni sensore/motore
//                             stato + cosa controllare se non funziona
//   tank                      stato tanica (livello, soglia, verdetto)
//   tank <cm>                 imposta soglia tanica-vuota (persistita)
//   rtc                       epoch corrente + validita' oscillatore
//   rtc set <epoch>           imposta l'ora del DS3232 (epoch Unix in s)
//   mode <idle|light|shadow>  cambio modalita' operativa
//   motor <f|b|l|r> <ms>      test motori (max 5000 ms)
//   motortest                 sequenza guidata f/b/l/r per verificare versi/mapping
//   calib <left> <right>      calibrazione PWM motori 0..255 (persistita)
//   pump <ms>                 test pompa (max 60000 ms, blocco se tanica vuota)
//   standalone <on|off>       sospende il deadman Hub per test a banco
//   reboot                    soft reset via WDT
class Cli {
public:
    Cli();
    // tick va chiamato nel loop principale; legge da Serial senza bloccare.
    void tick(Movement& mv, Sensors& sn, Pump& pp,
              Persistence& ps, SystemStatus& sys);

private:
    void execute(const char* line, Movement& mv, Sensors& sn, Pump& pp,
                 Persistence& ps, SystemStatus& sys);
    void printHelp();
    void printStatus(Movement& mv, Pump& pp, SystemStatus& sys);
    void printStats(Persistence& ps);
    void printConfig(Persistence& ps);
    void printSensors(Sensors& sn);
    void printDiag(Sensors& sn, Movement& mv, Pump& pp, Persistence& ps, SystemStatus& sys);
    void printTank(Sensors& sn, Pump& pp, Persistence& ps);

    static const size_t BUF_SIZE = 64;
    char  buf[BUF_SIZE];
    size_t pos;
};

#endif // CLI_H
