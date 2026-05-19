#include "Cli.h"
#include "Movement.h"
#include "Sensors.h"
#include "Pump.h"
#include "Persistence.h"
#include "SystemStatus.h"

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
    if (strcmp(line, "status")  == 0) { printStatus(mv, sys); return; }
    if (strcmp(line, "stats")   == 0) { printStats(ps);       return; }
    if (strcmp(line, "config")  == 0) { printConfig(ps);      return; }
    if (strcmp(line, "sensors") == 0) { printSensors(sn);     return; }
    if (strcmp(line, "reboot")  == 0) {
        Serial.println(F("[CLI] reboot requested"));
        sys.softResetRequested = true;
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
        if (pp.start((uint32_t)ms, ps.getStats())) {
            Serial.print(F("[CLI] pump on for "));
            Serial.print(ms);
            Serial.println(F(" ms"));
        } else {
            Serial.println(F("[CLI] pump busy or invalid duration"));
        }
        return;
    }

    Serial.print(F("[CLI] unknown: '"));
    Serial.print(line);
    Serial.println(F("' (try 'help')"));
}

void Cli::printHelp() {
    Serial.println(F("--- SmartVase CLI ---"));
    Serial.println(F("help                    questo menu"));
    Serial.println(F("status                  modalita' + stato runtime"));
    Serial.println(F("stats                   statistiche cumulative EEPROM"));
    Serial.println(F("config                  configurazione corrente"));
    Serial.println(F("sensors                 ultime letture sensori"));
    Serial.println(F("mode <idle|light|shadow>  cambia modalita'"));
    Serial.println(F("motor <f|b|l|r> <ms>    test motori (max 5000 ms)"));
    Serial.println(F("pump <ms>               test pompa (max 60000 ms)"));
    Serial.println(F("reboot                  soft reset"));
}

void Cli::printStatus(Movement& mv, SystemStatus& sys) {
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
    Serial.print(F("degradedMode=")); Serial.println(sys.degradedModeActive ? F("YES") : F("NO"));
    if (sys.degradedModeActive) {
        Serial.print(F("  reason="));
        Serial.println(sys.degradedReason);
    }
    Serial.print(F("hubMissing="));     Serial.println(sys.hubIsMissing ? F("YES") : F("NO"));
    Serial.print(F("lowMemory="));      Serial.println(sys.lowMemoryDetected ? F("YES") : F("NO"));
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
