/*
 * =================================================================
 * SmartVase - Platform Controller (Arduino Mega)
 * Versione 5.1 — 2026-06-11 (hardening pre-bring-up)
 * =================================================================
 *  - 6 sensori HC-SR04 con pin del nuovo PIN map (driver locale Ultrasonic)
 *  - Forcella umidita' suolo su A0, LDR spostato su A1
 *  - Pompa irrigazione via rele' D10 con protezione tanica vuota (US4)
 *  - RTC DS3232 (I2C 0x68) per timestamp epoch nei messaggi
 *  - WDT 4s, doppio slot EEPROM con CRC16, log queue
 *  - Comunicazione Serial1 Protobuf+framing verso ESP32 Hub
 *  - Scheduler non-bloccante per telemetria/heartbeat/log
 *  - Modalita' standalone (CLI) per test a banco senza Hub
 * =================================================================
 */

#include <Arduino.h>
#include <avr/wdt.h>

#include "Movement.h"
#include "Sensors.h"
#include "Communication.h"
#include "Persistence.h"
#include "Pump.h"
#include "SystemStatus.h"
#include "Cli.h"

// =================================================================
// Oggetti globali
// =================================================================
Movement      movement;
Sensors       sensors;
Communication comm;
Persistence   persistence;
Pump          pump;
Cli           cli;
SystemStatus  systemStatus = {false, false, false, true, false, false, "", "MEGA_01"};

// =================================================================
// Variabili di stato
// =================================================================
uint8_t mcusr_mirror __attribute__ ((section (".noinit")));

// Scheduler (intervalli in ms)
#define INTERVAL_FAST_TELEMETRY_MS    1000UL
#define INTERVAL_DEEP_TELEMETRY_MS   30000UL
#define INTERVAL_HEARTBEAT_MS         5000UL
#define INTERVAL_LOG_DRAIN_MS          200UL
#define HUB_DEADMAN_TIMEOUT_MS      120000UL

// Hysteresis SRAM: si entra in degraded sotto LOW; si esce solo sopra HIGH.
// L'isteresi evita oscillazioni rapide quando la RAM libera fluttua intorno a LOW.
#define LOW_RAM_THRESHOLD_BYTES        800
#define HIGH_RAM_THRESHOLD_BYTES      1200

unsigned long lastFastTelemetryMs  = 0;
unsigned long lastDeepTelemetryMs  = 0;
unsigned long lastHeartbeatMs      = 0;
unsigned long lastLogDrainMs       = 0;

// =================================================================
// Utility
// =================================================================
int freeRam() {
    extern int __heap_start, *__brkval;
    int v;
    return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) &__brkval);
}

// Disabilita il WDT subito dopo il reset hardware (prima di setup()) e
// salva il valore di MCUSR per distinguere watchdog reset da power-on.
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) {
    mcusr_mirror = MCUSR;
    MCUSR = 0;
    wdt_disable();
}

void enterDegradedMode(const char* reason) {
    if (systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = true;
    strncpy(systemStatus.degradedReason, reason, sizeof(systemStatus.degradedReason) - 1);
    systemStatus.degradedReason[sizeof(systemStatus.degradedReason) - 1] = '\0';
    movement.setTargetMode(CPP_IDLE);
    movement.stopMotors(persistence.getStats());
    pump.stop(persistence.getStats());
    comm.logEvent(Log_LogLevel_CRITICAL, "degraded_mode", reason,
                  systemStatus.deviceId, persistence.getStats());
    persistence.saveStats(true);
}

void exitDegradedMode() {
    if (!systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = false;
    systemStatus.degradedReason[0] = '\0';
    comm.logEvent(Log_LogLevel_INFO, "degraded_exit", "recovered",
                  systemStatus.deviceId, persistence.getStats());
}

void performSoftReset() {
    persistence.saveStats(true);
    comm.logEvent(Log_LogLevel_WARN, "soft_reset", "via_command",
                  systemStatus.deviceId, persistence.getStats());
    delay(100);
    wdt_enable(WDTO_15MS);
    while (true) { /* wait for WDT */ }
}

// =================================================================
// Setup
// =================================================================
void setup() {
    Serial.begin(115200);   // USB / CLI debug
    Serial1.begin(115200);  // Verso ESP32 Hub (Protobuf framing)

    Serial.println(F("\n[SmartVase] Platform Controller v" SMARTVASE_FW_VERSION " boot"));
    Serial.println(F("[SmartVase] CLI pronta: digita 'help'. Per test senza Hub: 'standalone on'"));

    // Carica config + stats da EEPROM (con fallback ai default se corrotti)
    persistence.loadConfig();
    persistence.loadStats();

    // Diagnosi del motivo del reset usando MCUSR catturato in init3.
    if (mcusr_mirror & (1 << WDRF)) {
        persistence.getStats().watchdog_resets++;
        persistence.saveStats(true);
        comm.logEvent(Log_LogLevel_CRITICAL, "reboot_wdt", "watchdog_reset",
                      systemStatus.deviceId, persistence.getStats());
    } else {
        comm.logEvent(Log_LogLevel_INFO, "boot", "power_on_reset",
                      systemStatus.deviceId, persistence.getStats());
    }

    // Da qui in poi il watchdog vigila (resetta dopo 4s di stallo).
    wdt_enable(WDTO_4S);

    sensors.init();
    movement.init();
    pump.init();
    comm.init();

#if BME680_ENABLED
    if (!sensors.getBMEStatus()) {
        systemStatus.bmeSensorError = true;
        comm.logEvent(Log_LogLevel_ERROR, "sensor_init_fail", "BME680",
                      systemStatus.deviceId, persistence.getStats());
    }
#endif
    if (!sensors.getRTCStatus()) {
        comm.logEvent(Log_LogLevel_WARN, "sensor_init_fail", "DS3232_RTC",
                      systemStatus.deviceId, persistence.getStats());
        Serial.println(F("[SmartVase] RTC non rilevato su I2C 0x68"));
    } else if (sensors.rtcOscStopped()) {
        // Oscillatore fermo = ora non valida (modulo nuovo o batteria scarica).
        comm.logEvent(Log_LogLevel_WARN, "rtc_osf", "time_not_set",
                      systemStatus.deviceId, persistence.getStats());
        Serial.println(F("[SmartVase] RTC presente ma ora NON valida: usa 'rtc set <epoch>'"));
    }

    comm.logEvent(Log_LogLevel_INFO, "system_boot", "platform_ready",
                  systemStatus.deviceId, persistence.getStats());
    Serial.println(F("[SmartVase] setup complete."));
}

// =================================================================
// Main loop (non bloccante)
// =================================================================
void loop() {
    wdt_reset();
    const unsigned long now = millis();

    // 0) CLI debug su USB Serial (Serial != Serial1)
    cli.tick(movement, sensors, pump, persistence, systemStatus);

    // 1) RX seriale (drain + eventuale esecuzione comandi)
    comm.handleSerial(movement, persistence, sensors, pump, systemStatus);

    // 2) Campionamento sensori (round-robin HC-SR04 + ADC)
    sensors.sampleSensors();

    // 3) Pompa: spegnimento automatico a fine timer + protezione tanica.
    //    Se durante l'irrigazione il livello (filtrato EMA) scende oltre la
    //    soglia, o US4 smette di dare letture valide, si ferma subito:
    //    mai far girare la pompa a secco.
    pump.tick(persistence.getStats());
    if (pump.isActive() &&
        sensors.tankLooksEmpty(persistence.getConfig().tank_empty_cm)) {
        pump.stop(persistence.getStats());
        comm.logEvent(Log_LogLevel_WARN, "pump_stop", "tank_empty_or_fault",
                      systemStatus.deviceId, persistence.getStats());
    }

    // 4) State machine movimento
    ObstacleView ov;
    ov.top         = sensors.getTopDist();
    ov.front_right = sensors.getFrontRightDist();
    ov.front_left  = sensors.getFrontLeftDist();
    ov.left        = sensors.getLeftDist();
    ov.right       = sensors.getRightDist();
    movement.handleMovementSM(ov, sensors.getLux(),
                              persistence.getConfig(), persistence.getStats(),
                              systemStatus.degradedModeActive);

    // 5) Scheduler trasmissioni periodiche
    if (now - lastFastTelemetryMs >= INTERVAL_FAST_TELEMETRY_MS) {
        TelemetryFast tf = sensors.buildFastTelemetry(movement.getCurrentState(),
                                                      systemStatus.deviceId);
        comm.sendFastTelemetry(tf);
        lastFastTelemetryMs = now;
    }
    if (now - lastDeepTelemetryMs >= INTERVAL_DEEP_TELEMETRY_MS) {
        TelemetryDeep td = sensors.buildDeepTelemetry(persistence.getStats(),
                                                      systemStatus.deviceId);
        comm.sendDeepTelemetry(td);
        lastDeepTelemetryMs = now;
    }
    if (now - lastHeartbeatMs >= INTERVAL_HEARTBEAT_MS) {
        comm.sendHeartbeat(now / 1000UL, systemStatus.degradedModeActive,
                           systemStatus.deviceId);
        lastHeartbeatMs = now;
    }
    if (now - lastLogDrainMs >= INTERVAL_LOG_DRAIN_MS) {
        comm.drainLogQueue(systemStatus.deviceId);
        lastLogDrainMs = now;
    }

    // 6) EEPROM stats (throttled internamente)
    persistence.saveStats(false);

    // 7) Health checks
    const int freeBytes = freeRam();
    if (freeBytes < LOW_RAM_THRESHOLD_BYTES) {
        if (!systemStatus.lowMemoryDetected) {
            systemStatus.lowMemoryDetected = true;
            enterDegradedMode("low_sram");
        }
    } else if (systemStatus.lowMemoryDetected && freeBytes >= HIGH_RAM_THRESHOLD_BYTES) {
        systemStatus.lowMemoryDetected = false;
        // Recover solo se nessun altro motivo tiene attivo degraded mode.
        if (!systemStatus.hubIsMissing) exitDegradedMode();
    }

    // Deadman Hub: sospeso in modalita' standalone (test a banco senza Hub).
    if (systemStatus.standaloneMode) {
        if (systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = false;
            if (!systemStatus.lowMemoryDetected) exitDegradedMode();
        }
    } else if (now - comm.getLastHubMessageMs() > HUB_DEADMAN_TIMEOUT_MS) {
        if (!systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = true;
            enterDegradedMode("hub_missing");
        }
    } else if (systemStatus.hubIsMissing) {
        systemStatus.hubIsMissing = false;
        if (!systemStatus.lowMemoryDetected) exitDegradedMode();
    }

    // 8) Soft reset richiesto via comando
    if (systemStatus.softResetRequested) {
        performSoftReset();
    }
}
