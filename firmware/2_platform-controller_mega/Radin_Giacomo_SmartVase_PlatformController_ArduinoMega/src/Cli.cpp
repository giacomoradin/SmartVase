/*!
    @file   Cli.cpp

    @ingroup MegaCli

    @brief  Implementazione del parser/dispatcher CLI (vedi Cli.h per la tabella comandi).

    @date   2026-05-20

    @author Giacomo Radin
*/

#include "Cli.h"
#include "Movement.h"
#include "Sensors.h"
#include "Pump.h"
#include "Persistence.h"
#include "SystemStatus.h"

/*! @brief RAM SRAM libera in byte, definita in main.cpp (usata da `status`/`diag`). */
extern int freeRam();

/*!
    @brief    Nome leggibile (stringa in flash) dello stato di movimento.
    @details  Usato dal comando `diag` per stampare lo stato corrente della FSM senza
              tenere una stringa duplicata in RAM (`F()` la mette in PROGMEM su AVR).
    @param[in] s Stato di movimento da convertire.
    @return   Puntatore a stringa flash con il nome dello stato.
*/
static const __FlashStringHelper* movStateName(CppMovementState s) {
    switch (s) {
        case CPP_M_MOVING:          return F("MOVING");
        case CPP_M_AVOID_START:     return F("AVOID_START");
        case CPP_M_AVOID_REVERSING: return F("AVOID_REVERSING");
        case CPP_M_AVOID_TURNING:   return F("AVOID_TURNING");
        case CPP_M_STUCK:           return F("STUCK");
        case CPP_M_IDLE:
        default:                    return F("IDLE");
    }
}

/*!
    @brief    Stampa una riga di diagnostica per una sonda ad ultrasuoni.
    @details  Usata dal comando `diag` per ciascuna delle 6 sonde HC-SR04: mostra
              pin TRIG/ECHO, valore letto e un suggerimento di troubleshooting se
              la lettura è `NaN` (nessun eco, possibile problema di cablaggio/alimentazione).
    @param[in] label Etichetta della sonda (stringa flash, es. "US1 top").
    @param[in] trig  Numero del pin TRIG (solo per la stampa diagnostica).
    @param[in] echo  Numero del pin ECHO (solo per la stampa diagnostica).
    @param[in] cm    Distanza misurata in cm (`NaN` se la lettura non è valida).
*/
static void diagUs(const __FlashStringHelper* label, uint8_t trig, uint8_t echo, float cm) {
    Serial.print(F("  "));
    Serial.print(label);
    Serial.print(F(" (TRIG ")); Serial.print(trig);
    Serial.print(F("/ECHO "));  Serial.print(echo);
    Serial.print(F(") = "));
    if (isnan(cm)) {
        Serial.println(F("nan  [!! NESSUNA LETTURA: trig/echo invertiti, sonda non alimentata o GND comune mancante -> verifica col PINS CSV]"));
    } else {
        Serial.print(cm); Serial.println(F(" cm  [ok]"));
    }
}

Cli::Cli() : pos(0) { buf[0] = '\0'; }

void Cli::tick(Movement& mv, Sensors& sn, Pump& pp,
               Persistence& ps, SystemStatus& sys) {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) execute(buf, mv, sn, pp, ps, sys);
            pos = 0;
            buf[0] = '\0';
            Serial.print(F("> "));
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = c;
        } else {
            // Riga troppo lunga: scarta.
            pos = 0;
            buf[0] = '\0';
            Serial.println(F("[CLI] line too long, discarded"));
        }
    }
}

void Cli::execute(const char* line, Movement& mv, Sensors& sn, Pump& pp,
                  Persistence& ps, SystemStatus& sys) {
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { printHelp(); return; }
    if (strcmp(line, "version") == 0) {
        Serial.println(F("SmartVase Platform Controller v" SMARTVASE_FW_VERSION
                         " (" __DATE__ " " __TIME__ ")"));
        return;
    }
    if (strcmp(line, "status")  == 0) { printStatus(mv, pp, sys);  return; }
    if (strcmp(line, "stats")   == 0) { printStats(ps);            return; }
    if (strcmp(line, "config")  == 0) { printConfig(ps);           return; }
    if (strcmp(line, "sensors") == 0) { printSensors(sn);          return; }
    if (strcmp(line, "diag")    == 0) { printDiag(sn, mv, pp, ps, sys); return; }
    if (strcmp(line, "motortest") == 0) {
        if (sys.degradedModeActive) {
            Serial.println(F("[CLI] impossibile: degraded mode attivo (motori bloccati)"));
            return;
        }
        DeviceConfig& c = ps.getConfig();
        Serial.println(F("[motortest] RUOTE SOLLEVATE da terra! Sequenza f/b/l/r ~800 ms ciascuna:"));
        Serial.println(F("  AVANTI   (entrambe le ruote in avanti)"));   mv.testMove('f', 800, c);
        Serial.println(F("  INDIETRO (entrambe indietro)"));            mv.testMove('b', 800, c);
        Serial.println(F("  SINISTRA (rotazione verso sinistra)"));     mv.testMove('l', 800, c);
        Serial.println(F("  DESTRA   (rotazione verso destra)"));       mv.testMove('r', 800, c);
        Serial.println(F("[motortest] fine. Una ruota gira al contrario -> inverti i suoi 2 fili."));
        Serial.println(F("  L/R scambiate -> scambia i gruppi pin in Movement.cpp; marcia storta -> 'calib'."));
        return;
    }
    if (strcmp(line, "tank")    == 0) { printTank(sn, pp, ps);     return; }
    if (strcmp(line, "reboot")  == 0) {
        Serial.println(F("[CLI] reboot requested"));
        sys.softResetRequested = true;
        return;
    }

    // tank <cm> — soglia tanica-vuota, persistita in EEPROM
    if (strncmp(line, "tank ", 5) == 0) {
        int cm;
        if (sscanf(line + 5, "%d", &cm) != 1 || cm < 3 || cm > 120) {
            Serial.println(F("[CLI] usage: tank <cm 3..120>"));
            return;
        }
        ps.getConfig().tank_empty_cm = (uint16_t)cm;
        ps.saveConfig(true);
        Serial.print(F("[CLI] tank_empty_cm = "));
        Serial.println(cm);
        printTank(sn, pp, ps);
        return;
    }

    // rtc / rtc set <epoch>
    if (strcmp(line, "rtc") == 0) {
        Serial.print(F("rtc_ok      = ")); Serial.println(sn.getRTCStatus() ? F("YES") : F("NO"));
        Serial.print(F("epoch_s     = ")); Serial.println(sn.getEpoch());
        Serial.print(F("time_valid  = ")); Serial.println(sn.rtcOscStopped() ? F("NO (usa 'rtc set <epoch>')") : F("YES"));
        return;
    }
    if (strncmp(line, "rtc set ", 8) == 0) {
        unsigned long epoch;
        if (sscanf(line + 8, "%lu", &epoch) != 1 || epoch < 1700000000UL) {
            Serial.println(F("[CLI] usage: rtc set <epoch unix in secondi>"));
            return;
        }
        if (sn.setEpoch((uint32_t)epoch)) {
            Serial.print(F("[CLI] RTC impostato a epoch "));
            Serial.println(epoch);
        } else {
            Serial.println(F("[CLI] RTC write FAILED (chip assente?)"));
        }
        return;
    }

    // mode <idle|light|shadow>
    if (strncmp(line, "mode ", 5) == 0) {
        const char* arg = line + 5;
        if      (strcmp(arg, "idle")   == 0) mv.setTargetMode(CPP_IDLE);
        else if (strcmp(arg, "light")  == 0) mv.setTargetMode(CPP_LIGHT);
        else if (strcmp(arg, "shadow") == 0) mv.setTargetMode(CPP_SHADOW);
        else { Serial.println(F("[CLI] usage: mode <idle|light|shadow>")); return; }
        Serial.print(F("[CLI] mode set to "));
        Serial.println(arg);
        return;
    }

    // motor <f|b|l|r> <ms>
    if (strncmp(line, "motor ", 6) == 0) {
        char dir;
        int ms;
        if (sscanf(line + 6, "%c %d", &dir, &ms) != 2 || ms < 0) {
            Serial.println(F("[CLI] usage: motor <f|b|l|r> <ms>"));
            return;
        }
        if (sys.degradedModeActive) {
            Serial.println(F("[CLI] cannot move: degraded mode active"));
            return;
        }
        mv.testMove(dir, (uint16_t)ms, ps.getConfig());
        Serial.println(F("[CLI] motor test done"));
        return;
    }

    // calib <left> <right> — PWM 0..255, persistita in EEPROM
    if (strncmp(line, "calib ", 6) == 0) {
        int l, r;
        if (sscanf(line + 6, "%d %d", &l, &r) != 2 ||
            l < 0 || l > 255 || r < 0 || r > 255) {
            Serial.println(F("[CLI] usage: calib <left 0..255> <right 0..255>"));
            return;
        }
        ps.getConfig().motorCalibLeft  = (uint8_t)l;
        ps.getConfig().motorCalibRight = (uint8_t)r;
        ps.saveConfig(true);
        Serial.print(F("[CLI] motorCalib L/R = "));
        Serial.print(l); Serial.print('/'); Serial.println(r);
        return;
    }

    // pump <ms>
    if (strncmp(line, "pump ", 5) == 0) {
        int ms;
        if (sscanf(line + 5, "%d", &ms) != 1 || ms <= 0) {
            Serial.println(F("[CLI] usage: pump <ms>"));
            return;
        }
        if (sys.degradedModeActive) {
            Serial.println(F("[CLI] cannot run pump: degraded mode active"));
            return;
        }
        // Stessa protezione tanica del comando remoto water.
        if (sn.tankLooksEmpty(ps.getConfig().tank_empty_cm)) {
            float wl = sn.getWaterLevel();
            if (isnan(wl)) {
                Serial.println(F("[CLI] BLOCCATO: US4 senza lettura valida (tank_sensor_fault)"));
            } else {
                Serial.print(F("[CLI] BLOCCATO: tanica vuota ("));
                Serial.print(wl);
                Serial.print(F(" cm > soglia "));
                Serial.print(ps.getConfig().tank_empty_cm);
                Serial.println(F(" cm)"));
            }
            Serial.println(F("[CLI] per tarare la soglia: 'tank <cm>'"));
            return;
        }
        if (pp.start((uint32_t)ms, ps.getStats())) {
            Serial.print(F("[CLI] pump on for "));
            Serial.print(ms);
            Serial.println(F(" ms"));
        } else {
            Serial.println(F("[CLI] pump busy or invalid duration"));
        }
        return;
    }

    // standalone <on|off> — sospende il deadman Hub per test a banco
    if (strncmp(line, "standalone ", 11) == 0) {
        const char* arg = line + 11;
        if (strcmp(arg, "on") == 0) {
            sys.standaloneMode = true;
            Serial.println(F("[CLI] standalone ON: deadman Hub sospeso"));
        } else if (strcmp(arg, "off") == 0) {
            sys.standaloneMode = false;
            Serial.println(F("[CLI] standalone OFF: deadman Hub attivo"));
        } else {
            Serial.println(F("[CLI] usage: standalone <on|off>"));
        }
        return;
    }

    Serial.print(F("[CLI] unknown: '"));
    Serial.print(line);
    Serial.println(F("' (try 'help')"));
}

void Cli::printHelp() {
    Serial.println(F("--- SmartVase CLI v" SMARTVASE_FW_VERSION " ---"));
    Serial.println(F("help                      questo menu"));
    Serial.println(F("version                   versione firmware"));
    Serial.println(F("status                    modalita' + stato runtime + RAM"));
    Serial.println(F("stats                     statistiche cumulative EEPROM"));
    Serial.println(F("config                    configurazione corrente"));
    Serial.println(F("sensors                   ultime letture sensori"));
    Serial.println(F("diag                      diagnostica guidata sensori/motori"));
    Serial.println(F("tank                      stato tanica acqua"));
    Serial.println(F("tank <cm>                 soglia tanica-vuota (3..120)"));
    Serial.println(F("rtc                       stato orologio DS3232"));
    Serial.println(F("rtc set <epoch>           imposta ora (epoch Unix)"));
    Serial.println(F("mode <idle|light|shadow>  cambia modalita'"));
    Serial.println(F("motor <f|b|l|r> <ms>      test motori (max 5000 ms)"));
    Serial.println(F("motortest                 sequenza guidata f/b/l/r"));
    Serial.println(F("calib <left> <right>      PWM motori 0..255"));
    Serial.println(F("pump <ms>                 test pompa (max 60000 ms)"));
    Serial.println(F("standalone <on|off>       test a banco senza Hub"));
    Serial.println(F("reboot                    soft reset"));
}

void Cli::printStatus(Movement& mv, Pump& pp, SystemStatus& sys) {
    Serial.print(F("fw_version="));
    Serial.println(F(SMARTVASE_FW_VERSION));
    Serial.print(F("targetMode="));
    switch (mv.getTargetMode()) {
        case CPP_LIGHT:  Serial.println(F("LIGHT"));  break;
        case CPP_SHADOW: Serial.println(F("SHADOW")); break;
        case CPP_IDLE:
        default:         Serial.println(F("IDLE"));   break;
    }
    Serial.print(F("movementState="));
    switch (mv.getCurrentState()) {
        case CPP_M_MOVING:          Serial.println(F("MOVING"));          break;
        case CPP_M_AVOID_START:     Serial.println(F("AVOID_START"));     break;
        case CPP_M_AVOID_REVERSING: Serial.println(F("AVOID_REVERSING")); break;
        case CPP_M_AVOID_TURNING:   Serial.println(F("AVOID_TURNING"));   break;
        case CPP_M_STUCK:           Serial.println(F("STUCK"));           break;
        case CPP_M_IDLE:
        default:                    Serial.println(F("IDLE"));            break;
    }
    Serial.print(F("pumpActive="));     Serial.println(pp.isActive() ? F("YES") : F("NO"));
    Serial.print(F("degradedMode="));   Serial.println(sys.degradedModeActive ? F("YES") : F("NO"));
    if (sys.degradedModeActive) {
        Serial.print(F("  reason="));
        Serial.println(sys.degradedReason);
    }
    Serial.print(F("standalone="));     Serial.println(sys.standaloneMode ? F("ON") : F("OFF"));
    Serial.print(F("hubMissing="));     Serial.println(sys.hubIsMissing ? F("YES") : F("NO"));
    Serial.print(F("lowMemory="));      Serial.println(sys.lowMemoryDetected ? F("YES") : F("NO"));
    Serial.print(F("freeRam="));        Serial.print(freeRam()); Serial.println(F(" B"));
    Serial.print(F("uptime_s="));       Serial.println(millis() / 1000UL);
    Serial.print(F("deviceId="));       Serial.println(sys.deviceId);
}

void Cli::printStats(Persistence& ps) {
    CumulativeStats& s = ps.getStats();
    Serial.println(F("--- stats ---"));
    Serial.print(F("watchdog_resets="));             Serial.println(s.watchdog_resets);
    Serial.print(F("total_irrigations="));           Serial.println(s.total_irrigations);
    Serial.print(F("total_irrigation_duration_s=")); Serial.println(s.total_irrigation_duration_s);
    Serial.print(F("total_motor_active_time_s="));   Serial.println(s.total_motor_active_time_s);
    Serial.print(F("light_seeking_sessions="));      Serial.println(s.light_seeking_sessions);
    Serial.print(F("shadow_seeking_sessions="));     Serial.println(s.shadow_seeking_sessions);
    Serial.print(F("obstacles_avoided="));           Serial.println(s.obstacles_avoided);
    Serial.print(F("escape_attempts="));             Serial.println(s.escape_attempts);
    Serial.print(F("stuck_events="));                Serial.println(s.stuck_events);
    Serial.print(F("bme_read_errors="));             Serial.println(s.bme_read_errors);
    Serial.print(F("log_overflows="));               Serial.println(s.log_overflows);
    Serial.print(F("pb_decode_failures="));          Serial.println(s.pb_decode_failures);
}

void Cli::printConfig(Persistence& ps) {
    DeviceConfig& c = ps.getConfig();
    Serial.println(F("--- config ---"));
    Serial.print(F("motorCalibLeft="));     Serial.println(c.motorCalibLeft);
    Serial.print(F("motorCalibRight="));    Serial.println(c.motorCalibRight);
    Serial.print(F("avoid_reverse_ms="));   Serial.println(c.avoid_reverse_ms);
    Serial.print(F("avoid_turn_ms="));      Serial.println(c.avoid_turn_ms);
    Serial.print(F("soil_dry_threshold=")); Serial.println(c.soil_dry_threshold);
    Serial.print(F("light_threshold="));    Serial.println(c.light_threshold);
    Serial.print(F("tank_empty_cm="));      Serial.println(c.tank_empty_cm);
}

void Cli::printSensors(Sensors& sn) {
    Serial.println(F("--- sensors ---"));
    Serial.print(F("US1 top         = ")); Serial.print(sn.getTopDist());        Serial.println(F(" cm"));
    Serial.print(F("US2 front_right = ")); Serial.print(sn.getFrontRightDist()); Serial.println(F(" cm"));
    Serial.print(F("US3 front_left  = ")); Serial.print(sn.getFrontLeftDist());  Serial.println(F(" cm"));
    Serial.print(F("US4 water_level = ")); Serial.print(sn.getWaterLevel());     Serial.println(F(" cm"));
    Serial.print(F("US5 left        = ")); Serial.print(sn.getLeftDist());       Serial.println(F(" cm"));
    Serial.print(F("US6 right       = ")); Serial.print(sn.getRightDist());      Serial.println(F(" cm"));
    Serial.print(F("lux            = "));  Serial.println(sn.getLux());
    Serial.print(F("soil_moisture  = "));  Serial.println(sn.getSoilMoisture());
    Serial.print(F("rtc_epoch_s    = "));  Serial.println(sn.getEpoch());
    Serial.print(F("bme_ok         = "));  Serial.println(sn.getBMEStatus() ? F("YES") : F("NO"));
    Serial.print(F("rtc_ok         = "));  Serial.println(sn.getRTCStatus() ? F("YES") : F("NO"));
}

void Cli::printDiag(Sensors& sn, Movement& mv, Pump& pp, Persistence& ps, SystemStatus& sys) {
    DeviceConfig& c = ps.getConfig();
    Serial.println(F("============== DIAGNOSTICA SmartVase =============="));

    // --- Ultrasuoni (pin = PINS - Sheet1.csv) ---
    Serial.println(F("[ULTRASUONI] nan = nessun eco -> cablaggio/alimentazione"));
    diagUs(F("US1 top        "), 33, 35, sn.getTopDist());
    diagUs(F("US2 front_right"), 26, 27, sn.getFrontRightDist());
    diagUs(F("US3 front_left "), 36, 37, sn.getFrontLeftDist());
    diagUs(F("US4 tanica     "), 50, 51, sn.getWaterLevel());
    diagUs(F("US5 left       "),  4,  5, sn.getLeftDist());
    diagUs(F("US6 right      "), 28, 29, sn.getRightDist());

    // --- Forcella umidita' suolo (A0) ---
    int soil = sn.getSoilMoisture();
    Serial.print(F("[FORCELLA SUOLO] A0 = ")); Serial.print(soil);
    Serial.print(F("  (soglia dry=")); Serial.print(c.soil_dry_threshold); Serial.println(F(")"));
    if (soil < 0)               Serial.println(F("  [!! lettura non valida]"));
    else if (soil <= 1 || soil >= 1022)
                                Serial.println(F("  [!! a fondo scala: forcella scollegata/in corto o non alimentata]"));
    else                        Serial.println(F("  [ok] immergi/asciuga la forcella e rilancia 'diag': il valore DEVE cambiare"));

    // --- Fotoresistore (A1) ---
    int lux = sn.getLux();
    Serial.print(F("[FOTORESISTORE] A1 = ")); Serial.print(lux);
    Serial.print(F("  (soglia light=")); Serial.print(c.light_threshold); Serial.println(F(")"));
    if (lux < 0)                Serial.println(F("  [!! lettura non valida]"));
    else if (lux <= 1 || lux >= 1022)
                                Serial.println(F("  [!! a fondo scala: LDR scollegato o partitore errato]"));
    else                        Serial.println(F("  [ok] copri/illumina l'LDR e rilancia: in LIGHT gira a dx se lux<soglia, in SHADOW a sx se lux>soglia"));

    // --- Motori (nessun feedback: si osservano) ---
    Serial.println(F("[MOTORI] nessun encoder -> vanno osservati a ruote sollevate"));
    Serial.print(F("  calib L/R = ")); Serial.print(c.motorCalibLeft);
    Serial.print('/'); Serial.println(c.motorCalibRight);
    Serial.println(F("  pin: L(ENA 6,IN1 43,IN2 45)  R(ENB 7,IN3 47,IN4 49)"));
    Serial.print(F("  movementState = ")); Serial.println(movStateName(mv.getCurrentState()));
    if (sys.degradedModeActive)
        Serial.println(F("  [!! DEGRADED: motori bloccati - vedi causa in [SISTEMA], usa 'standalone on' a banco]"));
    else
        Serial.println(F("  [ok] test: 'motor f 1000' poi b/l/r. Se una ruota gira al contrario: inverti i suoi 2 fili"));

    // --- Tanica / pompa ---
    float wl = sn.getWaterLevel();
    Serial.print(F("[POMPA/TANICA] US4 = "));
    if (isnan(wl)) Serial.print(F("nan")); else Serial.print(wl);
    Serial.print(F(" cm  soglia=")); Serial.print(c.tank_empty_cm);
    Serial.print(F("  pompa="));
    if (isnan(wl) || wl > (float)c.tank_empty_cm) Serial.println(F("BLOCCATA (tanica vuota/fault) [fail-safe]"));
    else                                          Serial.println(F("abilitata"));

    // --- RTC ---
    Serial.print(F("[RTC] ok=")); Serial.print(sn.getRTCStatus() ? F("YES") : F("NO"));
    Serial.print(F(" time_valid=")); Serial.print(sn.rtcOscStopped() ? F("NO") : F("YES"));
    Serial.print(F(" epoch=")); Serial.println(sn.getEpoch());
    if (sn.getRTCStatus() && sn.rtcOscStopped())
        Serial.println(F("  [!! ora non valida: 'rtc set <epoch>' (batteria CR2032?)]"));

    // --- Sistema ---
    Serial.print(F("[SISTEMA] freeRam=")); Serial.print(freeRam());
    Serial.print(F(" B  degraded=")); Serial.print(sys.degradedModeActive ? F("YES") : F("NO"));
    if (sys.degradedModeActive) { Serial.print(F(" (")); Serial.print(sys.degradedReason); Serial.print(F(")")); }
    Serial.println();
    Serial.print(F("  standalone=")); Serial.print(sys.standaloneMode ? F("ON") : F("OFF"));
    Serial.print(F("  hubMissing=")); Serial.println(sys.hubIsMissing ? F("YES") : F("NO"));
    Serial.println(F("=================================================="));
}

void Cli::printTank(Sensors& sn, Pump& pp, Persistence& ps) {
    float wl = sn.getWaterLevel();
    uint16_t thr = ps.getConfig().tank_empty_cm;
    Serial.println(F("--- tank ---"));
    Serial.print(F("water_level_cm = "));
    if (isnan(wl)) Serial.println(F("INVALID (US4 senza eco)"));
    else           Serial.println(wl);
    Serial.print(F("tank_empty_cm  = ")); Serial.println(thr);
    Serial.print(F("verdetto       = "));
    if (isnan(wl))            Serial.println(F("SENSOR FAULT -> pompa bloccata"));
    else if (wl > (float)thr) Serial.println(F("VUOTA -> pompa bloccata"));
    else                      Serial.println(F("OK -> pompa abilitata"));
    Serial.print(F("pumpActive     = ")); Serial.println(pp.isActive() ? F("YES") : F("NO"));
}
