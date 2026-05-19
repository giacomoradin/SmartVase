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
//   help / ?               lista comandi
//   status                 modalita' operativa + stato motori + degraded
//   stats                  statistiche cumulative dalla EEPROM
//   config                 configurazione corrente
//   sensors                ultime letture sensori
//   mode <idle|light|shadow>  cambio modalita' operativa
//   motor <f|b|l|r> <ms>   test motori (max 5000 ms)
//   pump <ms>              test pompa (max 60000 ms)
//   reboot                 soft reset via WDT
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
    void printStatus(Movement& mv, SystemStatus& sys);
    void printStats(Persistence& ps);
    void printConfig(Persistence& ps);
    void printSensors(Sensors& sn);

    static const size_t BUF_SIZE = 64;
    char  buf[BUF_SIZE];
    size_t pos;
};

#endif // CLI_H
