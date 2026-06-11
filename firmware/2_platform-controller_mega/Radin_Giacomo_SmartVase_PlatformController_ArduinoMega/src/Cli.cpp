#include "Cli.h"
#include "Movement.h"
#include "Sensors.h"
#include "Pump.h"
#include "Persistence.h"
#include "SystemStatus.h"

extern int freeRam(); // definito in main.cpp

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
    Serial.println(F("tank                      stato tanica acqua"));
    Serial.println(F("tank <cm>                 soglia tanica-vuota (3..120)"));
    Serial.println(F("rtc                       stato orologio DS3232"));
    Serial.println(F("rtc set <epoch>           imposta ora (epoch Unix)"));
    Serial.println(F("mode <idle|light|shadow>  cambia modalita'"));
    Serial.println(F("motor <f|b|l|r> <ms>      test motori (max 5000 ms)"));
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
