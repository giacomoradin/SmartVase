/*!
    @file   Cli.cpp

    @ingroup MegaCli

    @brief  Implementation of the CLI parser/dispatcher (see Cli.h for the command table).

    @date   2026-05-20

    @author Giacomo Radin
*/

#include "Cli.h"
#include <avr/wdt.h>
#include <Wire.h>
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
        Serial.println(F("nan  [!! NO READING: trig/echo swapped, probe unpowered or missing common GND -> check with PINS CSV]"));
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
                // Line too long: discard.
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

    // stats reset — reset cumulative statistics in EEPROM (historical counters
    // accumulated during debug sessions would skew the report).
    if (strcmp(line, "stats reset") == 0) {
        CumulativeStats& s = ps.getStats();
        const uint32_t magic = s.magic_number;
        const uint16_t wc    = s.write_counter;   // wear-leveling continuity
        memset(&s, 0, sizeof(s));
        s.magic_number  = magic;
        s.write_counter = wc;
        ps.saveStats(true);
        Serial.println(F("[CLI] cumulative statistics reset (EEPROM)"));
        return;
    }
    if (strcmp(line, "config")  == 0) { printConfig(ps);           return; }
    if (strcmp(line, "sensors") == 0) { printSensors(sn);          return; }
    if (strcmp(line, "diag")    == 0) { printDiag(sn, mv, pp, gl, ps, sys); return; }
    if (strcmp(line, "motortest") == 0) {
        if (sys.degradedModeActive) {
            Serial.println(F("[CLI] impossible: degraded mode active (motors blocked)"));
            return;
        }
        DeviceConfig& c = ps.getConfig();
        Serial.println(F("[motortest] WHEELS LIFTED off the ground! Sequence f/b/l/r ~800 ms each:"));
        Serial.println(F("  FORWARD  (both wheels forward)"));   mv.testMove('f', 800, c);
        Serial.println(F("  BACKWARD (both backward)"));            mv.testMove('b', 800, c);
        Serial.println(F("  LEFT     (rotation to the left)"));     mv.testMove('l', 800, c);
        Serial.println(F("  RIGHT    (rotation to the right)"));       mv.testMove('r', 800, c);
        Serial.println(F("[motortest] done. A wheel spins backwards -> swap its 2 wires."));
        Serial.println(F("  L/R scambiate -> scambia i gruppi pin in Movement.cpp; marcia storta -> 'calib'."));
        return;
    }
    if (strcmp(line, "tank")    == 0) { printTank(sn, pp, ps);     return; }
    if (strcmp(line, "reboot")  == 0) {
        Serial.println(F("[CLI] reboot requested"));
        sys.softResetRequested = true;
        return;
    }

    // i2cscan — scan of the hardware I2C bus (pins 20/21), with hints on the
    // expected devices. Avoids having to flash external scanner sketches.
    if (strcmp(line, "i2cscan") == 0) {
        Serial.println(F("[i2cscan] hardware bus SDA=20 / SCL=21, addresses 0x08-0x77..."));
        uint8_t found = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
            wdt_reset();   // on a faulted bus every probe waits the Wire timeout (25 ms)
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() != 0) continue;
            found++;
            Serial.print(F("  0x"));
            if (addr < 0x10) Serial.print('0');
            Serial.print(addr, HEX);
            switch (addr) {
                case 0x68: Serial.println(F("  DS3231/DS3232 RTC")); break;
                case 0x76:
                case 0x77: Serial.println(F("  BME680"));            break;
                default:
                    if (addr >= 0x50 && addr <= 0x57)
                        Serial.println(F("  AT24C32 EEPROM (onboard the HW-084 RTC module)"));
                    else
                        Serial.println(F("  (unknown)"));
                    break;
            }
        }
        if (found == 0) {
            Serial.println(F("[i2cscan] NO devices: check VCC/GND of the module,"));
            Serial.println(F("          SDA->pin 20 e SCL->pin 21 (non solo continuita': MAPPATURA)"));
        } else {
            Serial.print(F("[i2cscan] found "));
            Serial.print(found);
            Serial.println(F(" devices."));
            Serial.println(F("  HW-084 module: expected 0x68 (RTC) + 0x57 (EEPROM)."));
            Serial.println(F("  Only 0x57 without 0x68 -> bus ok but DS3231 chip mute (module/soldering)."));
        }
        return;
    }

    // tank <cm> — empty-tank threshold, persisted in EEPROM
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

    // light <adc> — light threshold (LIGHT/SHADOW seeking + UVA lights), persisted in EEPROM
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
        Serial.println(F("[CLI] note: used by both LIGHT/SHADOW seeking and UVA lights"));
        return;
    }

    // rtc / rtc set <epoch>
    if (strcmp(line, "rtc") == 0) {
        Serial.print(F("rtc_ok      = ")); Serial.println(sn.getRTCStatus() ? F("YES") : F("NO"));
        Serial.print(F("fake_clock  = ")); Serial.println(sn.isUsingFakeClock() ? F("YES (software, see 'rtc set')") : F("NO"));
        Serial.print(F("hub_sync    = "));
        if (sn.lastHubSyncMs() == 0) {
            Serial.println(F("NEVER (the Hub has not sent a valid NTP time yet)"));
        } else {
            Serial.print(F("YES, "));
            Serial.print((millis() - sn.lastHubSyncMs()) / 1000UL);
            Serial.println(F(" s ago (Hub heartbeat, proto v4.2)"));
        }
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
            Serial.print(F("[CLI] RTC chip absent/write failed: SOFTWARE clock activated at epoch "));
            Serial.println(epoch);
            Serial.println(F("[CLI] continua a scorrere finche' il Mega resta acceso; si perde al prossimo reset/spegnimento"));
        } else {
            Serial.print(F("[CLI] RTC (real chip) set to epoch "));
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

    // plant — current plant profile
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

    // care — autonomous-care layer status
    if (strcmp(line, "care") == 0) { printCare(cr, ps); return; }

    // care <on|off> — enables/disables autonomous care (persisted)
    if (strncmp(line, "care ", 5) == 0) {
        const char* arg = line + 5;
        if (strcmp(arg, "on") == 0) {
            ps.getConfig().care_enabled = 1;
            ps.saveConfig(true);
            cr.notifyEnabledChanged(mv, ps);
            Serial.println(F("[CLI] care ON: autonomous care active (profile: 'plant')"));
            Serial.println(F("[CLI] note: needs a valid time ('rtc') and, on the bench without the Hub, 'standalone on'"));
        } else if (strcmp(arg, "off") == 0) {
            ps.getConfig().care_enabled = 0;
            ps.saveConfig(true);
            cr.notifyEnabledChanged(mv, ps);
            Serial.println(F("[CLI] care OFF: manual control returns (mode/pump)"));
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
            Serial.println(F("[CLI] Cannot start: degraded mode active (motors blocked)"));
            return;
        }
        Serial.println(F("[CLI] Starting continuous motor test (10 minutes, forward) for electrical checks..."));
        Serial.println(F("[CLI] PRESS ANY KEY to stop the test and turn off the motors."));
        
        mv.moveForward(ps.getConfig());
        
        unsigned long startMs = millis();
        unsigned long durationMs = 10UL * 60UL * 1000UL; // 10 min
        unsigned long lastFeedbackSec = 0;
        bool stoppedByUser = false;
        
        while (millis() - startMs < durationMs) {
            wdt_reset(); // Impedisce il reset dell'Arduino (watchdog a 4 secondi)
            
            if (Serial.available()) {
                while (Serial.available()) Serial.read(); // Clear serial buffer
                stoppedByUser = true;
                break;
            }
            
            unsigned long elapsedSec = (millis() - startMs) / 1000UL;
            if (elapsedSec != lastFeedbackSec && elapsedSec % 10 == 0) {
                lastFeedbackSec = elapsedSec;
                Serial.print(F("[mfp0] Time: "));
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
            Serial.println(F("[CLI] Test mfp0 COMPLETED. Motors off."));
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
                Serial.println(F("[CLI] BLOCKED: US4 without valid reading (tank_sensor_fault)"));
            } else {
                Serial.print(F("[CLI] BLOCKED: empty tank ("));
                Serial.print(wl);
                Serial.print(F(" cm > threshold "));
                Serial.print(ps.getConfig().tank_empty_cm);
                Serial.println(F(" cm)"));
            }
            Serial.println(F("[CLI] to tune the threshold: 'tank <cm>'"));
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
            Serial.println(F("[CLI] standalone ON: Hub deadman suspended"));
        } else if (strcmp(arg, "off") == 0) {
            sys.standaloneMode = false;
            Serial.println(F("[CLI] standalone OFF: Hub deadman active"));
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
    Serial.println(F("help                      this menu"));
    Serial.println(F("version                   firmware version"));
    Serial.println(F("status                    modalita' + stato runtime + RAM"));
    Serial.println(F("stats                     cumulative EEPROM statistics"));
    Serial.println(F("stats reset               reset cumulative statistics"));
    Serial.println(F("config                    current configuration"));
    Serial.println(F("sensors                   last sensor readings"));
    Serial.println(F("diag                      guided sensors/motors diagnostics"));
    Serial.println(F("i2cscan                   I2C bus scan (RTC 0x68, EEPROM 0x50-57, BME 0x76)"));
    Serial.println(F("tank                      water tank state"));
    Serial.println(F("tank <cm>                 empty-tank threshold (3..120)"));
    Serial.println(F("light <adc>               light threshold (0..1023): seeking + UVA lights"));
    Serial.println(F("rtc                       DS3232 clock state"));
    Serial.println(F("rtc set <epoch>           set time (Unix epoch)"));
    Serial.println(F("mode <idle|light|shadow>  cambia modalita'"));
    Serial.println(F("plant                     current plant profile"));
    Serial.println(F("plant <shade|medium|sun>  apply plant preset (persisted)"));
    Serial.println(F("care                      autonomous care state + daily KPIs"));
    Serial.println(F("care <on|off>             autonomous plant care (default off)"));
    Serial.println(F("wall <left|right|off>     lateral wall-following (overrides seeking)"));
    Serial.println(F("motor <f|b|l|r> <ms>      motor test (max 60000 ms, wheels lifted)"));
    Serial.println(F("mfp0                      continuous forward motor test (10 min)"));
    Serial.println(F("motortest                 guided f/b/l/r sequence"));
    Serial.println(F("calib <left> <right>      motors PWM 0..255"));
    Serial.println(F("pump <ms>                 pump test (max 60000 ms)"));
    Serial.println(F("standalone <on|off>       bench test without Hub"));
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
    Serial.println(F("  (equivalent full light minutes/day)"));
    Serial.print(F("lux_high_adc         = ")); Serial.print(c.care_lux_high_adc);
    Serial.println(c.care_lux_high_adc > 1023 ? F("  (never shade due to too much light)") : F(""));
    Serial.print(F("soil_dry_adc         = ")); Serial.print(c.soil_dry_threshold);
    Serial.println(F("  (below: starts a watering cycle)"));
    Serial.print(F("soil_wet_adc         = ")); Serial.print(c.care_soil_wet_adc);
    Serial.println(F("  (above: cycle stops)"));
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
        Serial.println(F("suspended        = YES (manual override: resumes on its own)"));
    Serial.print(F("state            = ")); Serial.println(cr.stateName());
    Serial.print(F("light_budget     = ")); Serial.print(cr.budgetPct(c));
    Serial.print(F(" %  (target ")); Serial.print(c.care_light_target_min);
    Serial.println(F(" min full light)"));
    Serial.print(F("day_max_adc      = ")); Serial.print(cr.dayMaxAdc());
    Serial.println(F("  (LDR auto-calibration reference)"));
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
    Serial.println(F("============== SmartVase DIAGNOSTICS =============="));

    // --- Ultrasuoni (pin = PINS - Sheet1.csv) ---
    Serial.println(F("[ULTRASONICS] nan = no echo -> wiring/power"));
    diagUs(F("US1 top        "), 33, 35, sn.getTopDist());
    diagUs(F("US2 front_right"), 26, 27, sn.getFrontRightDist());
    diagUs(F("US3 front_left "), 36, 37, sn.getFrontLeftDist());
    diagUs(F("US4 tank       "), 50, 51, sn.getWaterLevel());
    diagUs(F("US5 left       "),  4,  5, sn.getLeftDist());
    diagUs(F("US6 right      "), 28, 29, sn.getRightDist());

    // --- Forcella umidita' suolo (A0) ---
    int soil = sn.getSoilMoisture();
    Serial.print(F("[SOIL FORK] A0 = ")); Serial.print(soil);
    Serial.print(F("  (dry threshold=")); Serial.print(c.soil_dry_threshold); Serial.println(F(")"));
    if (soil < 0)               Serial.println(F("  [!! invalid reading]"));
    else if (soil <= 1 || soil >= 1022)
                                Serial.println(F("  [!! full scale: fork disconnected/shorted or unpowered]"));
    else                        Serial.println(F("  [ok] immergi/asciuga la forcella e rilancia 'diag': il valore DEVE cambiare"));

    // --- Fotoresistore (A1) ---
    int lux = sn.getLux();
    Serial.print(F("[PHOTORESISTOR] A1 = ")); Serial.print(lux);
    Serial.print(F("  (light threshold=")); Serial.print(c.light_threshold); Serial.println(F(")"));
    if (lux < 0)                Serial.println(F("  [!! invalid reading]"));
    else if (lux <= 1 || lux >= 1022)
                                Serial.println(F("  [!! full scale: LDR disconnected or wrong divider]"));
    else                        Serial.println(F("  [ok] cover/illuminate the LDR and rerun: LIGHT turns right if lux<threshold, SHADOW left if lux>threshold"));

    // --- Luci di coltivazione (UVA, rele' canale 2 / D11, cablate su NC) ---
    bool timeValid = sn.timeIsValid();
    Serial.print(F("[UVA LIGHTS] growLight=")); Serial.print(gl.isOn() ? F("ON") : F("OFF"));
    Serial.print(F("  targetMode=")); Serial.println(mv.getTargetMode() == CPP_IDLE ? F("IDLE") : F("LIGHT/SHADOW"));
    Serial.print(F("  daylight window 06:00-20:00 -> timeValid=")); Serial.print(timeValid ? F("YES") : F("NO"));
    if (timeValid) {
        Serial.print(sn.isUsingFakeClock() ? F(" (SOFTWARE clock)") : F(" (real RTC)"));
        Serial.print(F("  time=")); Serial.print(hour((time_t)sn.getEpoch()));
        Serial.println(F(":xx"));
    } else {
        Serial.println(F(" [!! without a valid time the lights stay ALWAYS off: 'rtc set <epoch>']"));
    }
    Serial.println(F("  [ok] they turn on only if IDLE, lux < threshold AND inside the daylight window; wired on NC -> relay de-energized = ON"));

    // --- Motori (Pololu VNH5019, nessun encoder: si osservano) ---
    Serial.println(F("[MOTORS] Pololu VNH5019 - no encoder -> observe with lifted wheels"));
    Serial.print(F("  calib L/R = ")); Serial.print(c.motorCalibLeft);
    Serial.print('/'); Serial.println(c.motorCalibRight);
    Serial.println(F("  pin L: PWM7 INA41 INB43   R: PWM6 INA45 INB47   (EN/DIAG not wired)"));
    Serial.print(F("  fault L/R = ")); Serial.print(mv.faultLeft() ? F("!!FAULT") : F("n/a (EN not wired)"));
    Serial.print('/'); Serial.println(mv.faultRight() ? F("!!FAULT") : F("n/a (EN not wired)"));
    Serial.print(F("  movementState = ")); Serial.println(movStateName(mv.getCurrentState()));
    if (sys.degradedModeActive)
        Serial.println(F("  [!! DEGRADED: motors blocked - see cause in [SYSTEM], use 'standalone on' on the bench]"));
    else {
        Serial.println(F("  [ok] test: 'motor f 60000' poi b/l/r. Ruota al contrario -> inverti i suoi 2 fili (INA<->INB)"));
        Serial.println(F("  0V output (M1A/M1B)? -> 1) common GND Mega-shield  2) PWM/INA/INB pin mapping"));
    }

    // --- Tanica / pompa ---
    float wl = sn.getWaterLevel();
    Serial.print(F("[PUMP/TANK] US4 = "));
    if (isnan(wl)) Serial.print(F("nan")); else Serial.print(wl);
    Serial.print(F(" cm  threshold=")); Serial.print(c.tank_empty_cm);
    Serial.print(F("  pompa="));
    if (isnan(wl) || wl > (float)c.tank_empty_cm) Serial.println(F("BLOCKED (empty tank/fault) [fail-safe]"));
    else                                          Serial.println(F("enabled"));

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
    Serial.print(F("[SYSTEM] freeRam=")); Serial.print(freeRam());
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
    if (isnan(wl)) Serial.println(F("INVALID (US4 no echo)"));
    else           Serial.println(wl);
    Serial.print(F("tank_empty_cm  = ")); Serial.println(thr);
    Serial.print(F("verdict        = "));
    if (isnan(wl))            Serial.println(F("SENSOR FAULT -> pump blocked"));
    else if (wl > (float)thr) Serial.println(F("EMPTY -> pump blocked"));
    else                      Serial.println(F("OK -> pompa enabled"));
    Serial.print(F("pumpActive     = ")); Serial.println(pp.isActive() ? F("YES") : F("NO"));
}
