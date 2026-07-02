/*!
    @file   Cli.cpp

    @ingroup MegaCli

    @brief  Implementation of the CLI parser/dispatcher (see Cli.h for the command table).

    @date   2026-05-20

    @author Giacomo Radin
*/

#include "Cli.h"
#include <avr/wdt.h>
#include "Movement.h"
#include "Sensors.h"
#include "Pump.h"
#include "GrowLight.h"
#include "Care.h"
#include "Persistence.h"
#include "SystemStatus.h"
#include <TimeLib.h>

/*! @brief Free SRAM in bytes, defined in main.cpp (used by `status`/`diag`). */
extern int freeRam();

/*!
    @brief    Human-readable name (flash string) of a movement state.
    @details  Used by the `diag` command to print the current FSM state without
              keeping a duplicate string in RAM (`F()` puts it in PROGMEM on AVR).
    @param[in] s Movement state to convert.
    @return   Flash-string pointer with the state name.
*/
static const __FlashStringHelper* movStateName(CppMovementState s) {
    switch (s) {
        case CPP_M_MOVING:          return F("MOVING");
        case CPP_M_AVOID_START:     return F("AVOID_START");
        case CPP_M_AVOID_REVERSING: return F("AVOID_REVERSING");
        case CPP_M_AVOID_TURNING:   return F("AVOID_TURNING");
        case CPP_M_STUCK:           return F("STUCK");
        case CPP_M_SCAN_ROTATE:     return F("SCAN_ROTATE");
        case CPP_M_SCAN_ALIGN:      return F("SCAN_ALIGN");
        case CPP_M_IDLE:
        default:                    return F("IDLE");
    }
}

/*!
    @brief    Human-readable name (flash string) of a plant preset (CarePlantKind).
    @details  Used by `config`/`plant`; unknown codes fall back to "medium",
              consistent with carePresetProfile() in CarePolicy.h.
    @param[in] kind Preset code (0=shade, 1=medium, 2=sun).
    @return   Flash-string pointer with the preset name.
*/
static const __FlashStringHelper* plantKindName(uint8_t kind) {
    switch (kind) {
        case CARE_PLANT_SHADE: return F("shade");
        case CARE_PLANT_SUN:   return F("sun");
        case CARE_PLANT_MEDIUM:
        default:               return F("medium");
    }
}

/*!
    @brief    Prints one diagnostic line for an ultrasonic probe.
    @details  Used by the `diag` command for each of the 6 HC-SR04 probes: shows
              the TRIG/ECHO pins, the value read and a troubleshooting hint when
              the reading is `NaN` (no echo, possible wiring/power issue).
    @param[in] label Probe label (flash string, e.g. "US1 top").
    @param[in] trig  TRIG pin number (for the diagnostic printout only).
    @param[in] echo  ECHO pin number (for the diagnostic printout only).
    @param[in] cm    Measured distance in cm (`NaN` if the reading is not valid).
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

void Cli::tick(Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl, Care& cr,
               Persistence& ps, SystemStatus& sys) {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) execute(buf, mv, sn, pp, gl, cr, ps, sys);
            pos = 0;
            buf[0] = '\0';
            Serial.print(F("> "));
        } else if (c == 8 || c == 127) { // Backspace handling
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                Serial.print(F("\b \b")); // Erase character on terminal screen
            }
        } else if (c >= 32 && c <= 126) { // Printable ASCII only
            if (pos < BUF_SIZE - 1) {
                buf[pos++] = c;
            } else {
                // Riga troppo lunga: scarta.
                pos = 0;
                buf[0] = '\0';
                Serial.println(F("\n[CLI] line too long, discarded"));
                Serial.print(F("> "));
            }
        }
    }
}

void Cli::execute(const char* line, Movement& mv, Sensors& sn, Pump& pp, GrowLight& gl,
                  Care& cr, Persistence& ps, SystemStatus& sys) {
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { printHelp(); return; }
    if (strcmp(line, "version") == 0) {
        Serial.println(F("SmartVase Platform Controller v" SMARTVASE_FW_VERSION
                         " (" __DATE__ " " __TIME__ ")"));
        return;
    }
    if (strcmp(line, "status")  == 0) { printStatus(mv, pp, gl, cr, sys); return; }
    if (strcmp(line, "stats")   == 0) { printStats(ps);            return; }
    if (strcmp(line, "config")  == 0) { printConfig(ps);           return; }
    if (strcmp(line, "sensors") == 0) { printSensors(sn);          return; }
    if (strcmp(line, "diag")    == 0) { printDiag(sn, mv, pp, gl, ps, sys); return; }
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

    // light <adc> — soglia luminosita' (seeking LIGHT/SHADOW + luci UVA), persistita in EEPROM
    if (strncmp(line, "light ", 6) == 0) {
        int adc;
        if (sscanf(line + 6, "%d", &adc) != 1 || adc < 0 || adc > 1023) {
            Serial.println(F("[CLI] usage: light <adc 0..1023>"));
            return;
        }
        ps.getConfig().light_threshold = (uint16_t)adc;
        ps.saveConfig(true);
        Serial.print(F("[CLI] light_threshold = "));
        Serial.println(adc);
        Serial.println(F("[CLI] nota: usata sia dal seeking LIGHT/SHADOW sia dalle luci UVA"));
        return;
    }

    // rtc / rtc set <epoch>
    if (strcmp(line, "rtc") == 0) {
        Serial.print(F("rtc_ok      = ")); Serial.println(sn.getRTCStatus() ? F("YES") : F("NO"));
        Serial.print(F("fake_clock  = ")); Serial.println(sn.isUsingFakeClock() ? F("YES (software, vedi 'rtc set')") : F("NO"));
        Serial.print(F("epoch_s     = ")); Serial.println(sn.getEpoch());
        Serial.print(F("time_valid  = ")); Serial.println(sn.timeIsValid() ? F("YES") : F("NO (usa 'rtc set <epoch>')"));
        return;
    }
    if (strncmp(line, "rtc set ", 8) == 0) {
        unsigned long epoch;
        if (sscanf(line + 8, "%lu", &epoch) != 1 || epoch < 1700000000UL) {
            Serial.println(F("[CLI] usage: rtc set <epoch unix in secondi>"));
            return;
        }
        sn.setEpoch((uint32_t)epoch);
        if (sn.isUsingFakeClock()) {
            Serial.print(F("[CLI] chip RTC assente/scrittura fallita: clock SOFTWARE attivato a epoch "));
            Serial.println(epoch);
            Serial.println(F("[CLI] continua a scorrere finche' il Mega resta acceso; si perde al prossimo reset/spegnimento"));
        } else {
            Serial.print(F("[CLI] RTC (chip reale) impostato a epoch "));
            Serial.println(epoch);
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

    // plant — profilo pianta corrente
    if (strcmp(line, "plant") == 0) { printPlant(ps); return; }

    // plant <shade|medium|sun> — applica un preset pianta (persistito)
    if (strncmp(line, "plant ", 6) == 0) {
        const char* arg = line + 6;
        uint8_t kind;
        if      (strcmp(arg, "shade")  == 0) kind = CARE_PLANT_SHADE;
        else if (strcmp(arg, "medium") == 0) kind = CARE_PLANT_MEDIUM;
        else if (strcmp(arg, "sun")    == 0) kind = CARE_PLANT_SUN;
        else { Serial.println(F("[CLI] usage: plant <shade|medium|sun>")); return; }
        const PlantProfile p = carePresetProfile(kind);
        DeviceConfig& c = ps.getConfig();
        c.care_plant_kind        = kind;
        c.care_light_target_min  = p.light_target_min;
        c.care_lux_high_adc      = p.lux_high_adc;
        c.soil_dry_threshold     = p.soil_dry_adc;
        c.care_soil_wet_adc      = p.soil_wet_adc;
        c.care_dose_ms           = p.dose_ms;
        c.care_soak_min          = p.soak_min;
        c.care_max_doses         = p.max_doses_per_day;
        c.care_max_reloc         = p.max_reloc_per_day;
        c.care_growlight_max_min = p.grow_light_max_min;
        ps.saveConfig(true);
        Serial.print(F("[CLI] plant preset = "));
        Serial.println(arg);
        printPlant(ps);
        return;
    }

    // care — stato del layer di cura autonoma
    if (strcmp(line, "care") == 0) { printCare(cr, ps); return; }

    // care <on|off> — abilita/disabilita la cura autonoma (persistito)
    if (strncmp(line, "care ", 5) == 0) {
        const char* arg = line + 5;
        if (strcmp(arg, "on") == 0) {
            ps.getConfig().care_enabled = 1;
            ps.saveConfig(true);
            cr.notifyEnabledChanged(mv, ps);
            Serial.println(F("[CLI] care ON: cura autonoma attiva (profilo: 'plant')"));
            Serial.println(F("[CLI] nota: serve un'ora valida ('rtc') e, a banco senza Hub, 'standalone on'"));
        } else if (strcmp(arg, "off") == 0) {
            ps.getConfig().care_enabled = 0;
            ps.saveConfig(true);
            cr.notifyEnabledChanged(mv, ps);
            Serial.println(F("[CLI] care OFF: torna il controllo manuale (mode/pump)"));
        } else {
            Serial.println(F("[CLI] usage: care <on|off>"));
        }
        return;
    }

    // wall <left|right|off> — wall-following locale (usa i sensori laterali US5/US6)
    if (strncmp(line, "wall ", 5) == 0) {
        const char* arg = line + 5;
        if      (strcmp(arg, "left")  == 0) mv.setWallFollow(Movement::WALL_LEFT);
        else if (strcmp(arg, "right") == 0) mv.setWallFollow(Movement::WALL_RIGHT);
        else if (strcmp(arg, "off")   == 0) mv.setWallFollow(Movement::WALL_OFF);
        else { Serial.println(F("[CLI] usage: wall <left|right|off>")); return; }
        Serial.print(F("[CLI] wall-follow = "));
        Serial.println(arg);
        Serial.println(F("[CLI] nota: attivo solo in 'mode light/shadow' (durante M_MOVING); sovrascrive il seeking"));
        return;
    }

    // mfp0 — test continuo motori in avanti (10 min)
    if (strcmp(line, "mfp0") == 0) {
        if (sys.degradedModeActive) {
            Serial.println(F("[CLI] Impossibile avviare: modalita degraded attiva (motori bloccati)"));
            return;
        }
        Serial.println(F("[CLI] Avvio test motori continuativo (10 minuti, avanti) per verifiche elettriche..."));
        Serial.println(F("[CLI] PREMI UN TASTO QUALSIASI per interrompere il test e spegnere i motori."));
        
        mv.moveForward(ps.getConfig());
        
        unsigned long startMs = millis();
        unsigned long durationMs = 10UL * 60UL * 1000UL; // 10 min
        unsigned long lastFeedbackSec = 0;
        bool stoppedByUser = false;
        
        while (millis() - startMs < durationMs) {
            wdt_reset(); // Impedisce il reset dell'Arduino (watchdog a 4 secondi)
            
            if (Serial.available()) {
                while (Serial.available()) Serial.read(); // Svuota buffer seriale
                stoppedByUser = true;
                break;
            }
            
            unsigned long elapsedSec = (millis() - startMs) / 1000UL;
            if (elapsedSec != lastFeedbackSec && elapsedSec % 10 == 0) {
                lastFeedbackSec = elapsedSec;
                Serial.print(F("[mfp0] Tempo: "));
                Serial.print(elapsedSec / 60UL);
                Serial.print(F(" min "));
                Serial.print(elapsedSec % 60UL);
                Serial.println(F(" sec / 10 min"));
            }
            
            delay(50);
        }
        
        mv.stopMotors(ps.getStats());
        
        if (stoppedByUser) {
            Serial.println(F("[CLI] Test mfp0 INTERROTTO dall'utente. Motori spenti."));
        } else {
            Serial.println(F("[CLI] Test mfp0 COMPLETATO. Motori spenti."));
        }
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
        // Same tank protection as the remote water command.
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
    Serial.println(F("light <adc>               soglia luminosita' (0..1023): seeking + luci UVA"));
    Serial.println(F("rtc                       stato orologio DS3232"));
    Serial.println(F("rtc set <epoch>           imposta ora (epoch Unix)"));
    Serial.println(F("mode <idle|light|shadow>  cambia modalita'"));
    Serial.println(F("plant                     profilo pianta corrente"));
    Serial.println(F("plant <shade|medium|sun>  applica preset pianta (persistito)"));
    Serial.println(F("care                      stato cura autonoma + KPI giornalieri"));
    Serial.println(F("care <on|off>             cura autonoma della pianta (default off)"));
    Serial.println(F("wall <left|right|off>     wall-following laterale (sovrascrive seeking)"));
    Serial.println(F("motor <f|b|l|r> <ms>      test motori (max 5000 ms)"));
    Serial.println(F("mfp0                      test continuo motori avanti (10 min)"));
    Serial.println(F("motortest                 sequenza guidata f/b/l/r"));
    Serial.println(F("calib <left> <right>      PWM motori 0..255"));
    Serial.println(F("pump <ms>                 test pompa (max 60000 ms)"));
    Serial.println(F("standalone <on|off>       test a banco senza Hub"));
    Serial.println(F("reboot                    soft reset"));
}

void Cli::printStatus(Movement& mv, Pump& pp, GrowLight& gl, Care& cr, SystemStatus& sys) {
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
    Serial.print(F("growLight="));      Serial.println(gl.isOn() ? F("ON") : F("OFF"));
    Serial.print(F("careActive="));
    if (cr.isActive())                 Serial.println(cr.stateName());
    else if (cr.overrideActive())      Serial.println(F("PAUSED (manual override)"));
    else                               Serial.println(F("OFF"));
    Serial.print(F("wallFollow="));
    switch (mv.getWallFollow()) {
        case Movement::WALL_LEFT:  Serial.println(F("LEFT"));  break;
        case Movement::WALL_RIGHT: Serial.println(F("RIGHT")); break;
        default:                   Serial.println(F("OFF"));   break;
    }
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
    Serial.print(F("care_enabled="));       Serial.println(c.care_enabled ? F("YES") : F("NO"));
    Serial.print(F("plant_preset="));       Serial.println(plantKindName(c.care_plant_kind));
}

void Cli::printPlant(Persistence& ps) {
    DeviceConfig& c = ps.getConfig();
    Serial.println(F("--- plant profile ---"));
    Serial.print(F("preset               = ")); Serial.println(plantKindName(c.care_plant_kind));
    Serial.print(F("light_target_min     = ")); Serial.print(c.care_light_target_min);
    Serial.println(F("  (minuti di luce piena equivalente/giorno)"));
    Serial.print(F("lux_high_adc         = ")); Serial.print(c.care_lux_high_adc);
    Serial.println(c.care_lux_high_adc > 1023 ? F("  (mai ombra per troppa luce)") : F(""));
    Serial.print(F("soil_dry_adc         = ")); Serial.print(c.soil_dry_threshold);
    Serial.println(F("  (sotto: parte un ciclo di irrigazione)"));
    Serial.print(F("soil_wet_adc         = ")); Serial.print(c.care_soil_wet_adc);
    Serial.println(F("  (sopra: il ciclo si ferma)"));
    Serial.print(F("dose_ms              = ")); Serial.println(c.care_dose_ms);
    Serial.print(F("soak_min             = ")); Serial.println(c.care_soak_min);
    Serial.print(F("max_doses/day        = ")); Serial.println(c.care_max_doses);
    Serial.print(F("max_reloc/day        = ")); Serial.println(c.care_max_reloc);
    Serial.print(F("growlight_max_min    = ")); Serial.println(c.care_growlight_max_min);
}

void Cli::printCare(Care& cr, Persistence& ps) {
    DeviceConfig& c = ps.getConfig();
    Serial.println(F("--- care ---"));
    Serial.print(F("enabled          = ")); Serial.println(c.care_enabled ? F("YES") : F("NO"));
    if (cr.overrideActive())
        Serial.println(F("suspended        = YES (manual override: riprende da sola)"));
    Serial.print(F("state            = ")); Serial.println(cr.stateName());
    Serial.print(F("light_budget     = ")); Serial.print(cr.budgetPct(c));
    Serial.print(F(" %  (target ")); Serial.print(c.care_light_target_min);
    Serial.println(F(" min luce piena)"));
    Serial.print(F("day_max_adc      = ")); Serial.print(cr.dayMaxAdc());
    Serial.println(F("  (riferimento auto-calibrazione LDR)"));
    Serial.print(F("relocations      = ")); Serial.print(cr.relocationsToday());
    Serial.print(F(" / "));                 Serial.println(c.care_max_reloc);
    Serial.print(F("water_doses      = ")); Serial.print(cr.dosesToday());
    Serial.print(F(" / "));                 Serial.println(c.care_max_doses);
    Serial.print(F("dose_cycle       = ")); Serial.println(cr.doseCycleActive() ? F("ACTIVE") : F("idle"));
    Serial.print(F("soak_remaining_s = ")); Serial.println(cr.soakRemainingS());
    Serial.print(F("growlight_min    = ")); Serial.print(cr.growLightMinutesToday());
    Serial.print(F(" / "));                 Serial.println(c.care_growlight_max_min);
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

void Cli::printDiag(Sensors& sn, Movement& mv, Pump& pp, GrowLight& gl, Persistence& ps, SystemStatus& sys) {
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

    // --- Luci di coltivazione (UVA, rele' canale 2 / D11, cablate su NC) ---
    bool timeValid = sn.timeIsValid();
    Serial.print(F("[LUCI UVA] growLight=")); Serial.print(gl.isOn() ? F("ON") : F("OFF"));
    Serial.print(F("  targetMode=")); Serial.println(mv.getTargetMode() == CPP_IDLE ? F("IDLE") : F("LIGHT/SHADOW"));
    Serial.print(F("  finestra diurna 06:00-20:00 -> timeValid=")); Serial.print(timeValid ? F("YES") : F("NO"));
    if (timeValid) {
        Serial.print(sn.isUsingFakeClock() ? F(" (clock SOFTWARE)") : F(" (RTC reale)"));
        Serial.print(F("  ora=")); Serial.print(hour((time_t)sn.getEpoch()));
        Serial.println(F(":xx"));
    } else {
        Serial.println(F(" [!! senza ora valida le luci restano SEMPRE spente: 'rtc set <epoch>']"));
    }
    Serial.println(F("  [ok] si accendono solo se IDLE, lux < soglia E dentro la finestra diurna; cablate su NC -> rele' diseccitato = ACCESE"));

    // --- Motori (Pololu VNH5019, nessun encoder: si osservano) ---
    Serial.println(F("[MOTORI] Pololu VNH5019 - nessun encoder -> osservare a ruote sollevate"));
    Serial.print(F("  calib L/R = ")); Serial.print(c.motorCalibLeft);
    Serial.print('/'); Serial.println(c.motorCalibRight);
    Serial.println(F("  pin L: PWM7 INA41 INB43   R: PWM6 INA45 INB47   (EN/DIAG non cablato)"));
    Serial.print(F("  fault L/R = ")); Serial.print(mv.faultLeft() ? F("!!FAULT") : F("n/d (EN non cablato)"));
    Serial.print('/'); Serial.println(mv.faultRight() ? F("!!FAULT") : F("n/d (EN non cablato)"));
    Serial.print(F("  movementState = ")); Serial.println(movStateName(mv.getCurrentState()));
    if (sys.degradedModeActive)
        Serial.println(F("  [!! DEGRADED: motori bloccati - vedi causa in [SISTEMA], usa 'standalone on' a banco]"));
    else {
        Serial.println(F("  [ok] test: 'motor f 60000' poi b/l/r. Ruota al contrario -> inverti i suoi 2 fili (INA<->INB)"));
        Serial.println(F("  0V in uscita (M1A/M1B)? -> 1) GND comune Mega-shield  2) mapping pin PWM/INA/INB"));
    }

    // --- Tanica / pompa ---
    float wl = sn.getWaterLevel();
    Serial.print(F("[POMPA/TANICA] US4 = "));
    if (isnan(wl)) Serial.print(F("nan")); else Serial.print(wl);
    Serial.print(F(" cm  soglia=")); Serial.print(c.tank_empty_cm);
    Serial.print(F("  pompa="));
    if (isnan(wl) || wl > (float)c.tank_empty_cm) Serial.println(F("BLOCCATA (tanica vuota/fault) [fail-safe]"));
    else                                          Serial.println(F("abilitata"));

    // --- RTC ---
    Serial.print(F("[RTC] chip_ok=")); Serial.print(sn.getRTCStatus() ? F("YES") : F("NO"));
    Serial.print(F(" fake_clock=")); Serial.print(sn.isUsingFakeClock() ? F("YES") : F("NO"));
    Serial.print(F(" time_valid=")); Serial.print(sn.timeIsValid() ? F("YES") : F("NO"));
    Serial.print(F(" epoch=")); Serial.println(sn.getEpoch());
    if (!sn.getRTCStatus() && !sn.isUsingFakeClock())
        Serial.println(F("  [!! chip non rilevato su I2C: cablaggio, oppure 'rtc set <epoch>' attiva un clock software di fallback]"));
    else if (sn.getRTCStatus() && sn.rtcOscStopped() && !sn.isUsingFakeClock())
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
