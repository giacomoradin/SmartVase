/*!
    @file   main.cpp

    @ingroup MegaCore

    @brief  Setup/loop del Platform Controller (Arduino Mega) — "The Brawn".

    @details Versione 5.3:
             - 6 sensori HC-SR04 con pin del PIN map autoritativo (driver locale Ultrasonic)
             - Forcella umidità suolo su A0, LDR su A1
             - Pompa irrigazione via relè D10 con protezione tanica vuota (US4)
             - Luci di coltivazione (UVA) via relè D11: top-up di fine giornata quando
               la cura autonoma è attiva, regola legacy (IDLE + buio) altrimenti
             - Cura autonoma della pianta (layer L2, `care on`): budget luce giornaliero,
               light scan rotante, irrigazione dose-attesa-verifica, profili pianta (Care/CarePolicy)
             - RTC DS3232 (I2C 0x68) for epoch timestamp in messages
             - WDT 4s, doppio slot EEPROM con CRC16 (wear-leveling), log queue
             - Comunicazione Serial1 Protobuf+framing verso ESP32 Hub
             - Non-blocking scheduler for telemetry/heartbeat/log
             - Standalone mode (CLI) for bench test without Hub

    @date   2026-04-29

    @author Giacomo Radin
*/

#include <Arduino.h>
#include <avr/wdt.h>

#include "Movement.h"
#include "Sensors.h"
#include "Communication.h"
#include "Persistence.h"
#include "Pump.h"
#include "GrowLight.h"
#include "Care.h"
#include "SystemStatus.h"
#include "Cli.h"

// =================================================================
// Oggetti globali
// =================================================================
Movement      movement;     /**< Motor FSM, seeking, obstacle avoidance. */
Sensors       sensors;      /**< Sensor reading (ultrasonic, ADC, RTC, BME680). */
Communication comm;         /**< Protobuf serial framing to the Hub + log queue. */
Persistence   persistence;  /**< Config and statistics on dual-slot EEPROM. */
Pump          pump;         /**< Irrigation pump relay (non-blocking). */
GrowLight     growLight;    /**< UVA grow light relay (non-blocking). */
Care          care;         /**< Autonomous plant-care layer (light budget, watering, top-up). */
Cli           cli;          /**< Debug CLI over USB. */
SystemStatus  systemStatus = {false, false, false, true, false, false, false, "", "MEGA_01"}; /**< Shared system state (initialized with hubIsMissing=true until the Hub checks in). */

// =================================================================
// State variables
// =================================================================
/*! @brief Mirror of MCUSR captured in `.init3`, in RAM not cleared on reset: distinguishes watchdog reset from power-on. */
uint8_t mcusr_mirror __attribute__ ((section (".noinit")));

/*! @name Non-blocking scheduler intervals (ms)
 *  @{ */
#define INTERVAL_FAST_TELEMETRY_MS    1000UL   /**< TelemetryFast send period to the Hub. */
#define INTERVAL_DEEP_TELEMETRY_MS   30000UL   /**< TelemetryDeep send period to the Hub. */
#define INTERVAL_HEARTBEAT_MS         5000UL   /**< Heartbeat send period to the Hub. */
#define INTERVAL_LOG_DRAIN_MS          200UL   /**< Drain period for one log queue entry. */
#define HUB_DEADMAN_TIMEOUT_MS      120000UL   /**< Hub silence beyond which degraded mode is entered (`hub_missing`). */
/*! @} */

/*! @name RAM hysteresis thresholds for degraded mode (bytes)
 *  @details Degraded mode is entered below LOW; it can only be exited above HIGH. The
 *           hysteresis avoids rapid oscillation when free RAM fluctuates around LOW.
 *  @{ */
#define LOW_RAM_THRESHOLD_BYTES        800     /**< Below this free RAM, degraded mode is entered. */
#define HIGH_RAM_THRESHOLD_BYTES      1200     /**< Above this free RAM, degraded mode can be exited. */
/*! @} */

unsigned long lastFastTelemetryMs  = 0;  /**< Timestamp of the last TelemetryFast send. */
unsigned long lastDeepTelemetryMs  = 0;  /**< Timestamp of the last TelemetryDeep send. */
unsigned long lastHeartbeatMs      = 0;  /**< Timestamp of the last Heartbeat sent. */
unsigned long lastLogDrainMs       = 0;  /**< Timestamp of the last log queue drain. */

// =================================================================
// Utility
// =================================================================

/*!
    @brief    Estimates the remaining free SRAM (free heap relative to the stack).
    @details  Used by loop health checks and CLI status/diag commands
              to detect low RAM conditions before they cause stack corruption
              (AVR has no memory protection).
    @return   Estimated free bytes between the end of the heap (__brkval/__heap_start)
              and the current stack pointer.
*/
int freeRam() {
    extern int __heap_start, *__brkval;
    int v;
    return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) &__brkval);
}

/*!
    @brief    Disables the watchdog immediately after hardware reset, before setup().
    @details  Runs in the .init3 section (executed before the initialization of
              global variables), so even before main(). Necessary because
              on AVR the WDT remains active after a reset caused by the WDT itself: without
              this immediate disable, a slow boot (e.g. sensor initialization)
              could be interrupted by a second reset before
              setup() can call wdt_enable() with the final timeout.
              Also saves MCUSR in mcusr_mirror (.noinit section, survives
              reset) to allow setup() to distinguish a watchdog reset from
              a normal power-on.
*/
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) {
    mcusr_mirror = MCUSR;
    MCUSR = 0;
    wdt_disable();
}

/*!
    @brief    Enters degraded mode, disabling motors and pump.
    @details  Idempotent: if the system is already in degraded mode it does nothing (avoids
              relaunching log/stats saving on every loop iteration
              while the condition persists). Records the reason, stops
              physical actuation immediately (motors and pump), logs a
              CRITICAL event to the Hub and forces stats saving.
    @param[in] reason Short string identifying the cause (e.g. "low_sram", "hub_missing"),
                       copied to systemStatus.degradedReason.
*/
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

/*!
    @brief    Exits degraded mode and logs the recovery.
    @details  Should only be called when NO degraded mode conditions are
              active anymore (low RAM, Hub missing, etc.): callers in loop()
              already verify other causes before invoking it. Idempotent: if the
              system is not in degraded mode it does nothing.
*/
void exitDegradedMode() {
    if (!systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = false;
    systemStatus.degradedReason[0] = '\0';
    comm.logEvent(Log_LogLevel_INFO, "degraded_exit", "recovered",
                  systemStatus.deviceId, persistence.getStats());
}

/*!
    @brief    Performs a software reset via watchdog, on command request.
    @details  Saves stats, logs the event, then forces a voluntary reset
              by arming the WDT with a very short timeout (15 ms) and entering an
              infinite wait loop: there is no "clean" way to do a software reset
              on AVR, so the watchdog is deliberately exploited.
*/
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

/*!
    @brief    Firmware initialization: serials, EEPROM, sensors, actuators, watchdog.
    @details  Relevant order: loads config/stats from EEPROM before diagnosing
              the reset cause (needed to increment/save
              watchdog_resets), then enables the WDT (4 s) only after completing
              potentially slow initializations, to avoid risking a premature reset
              during the boot itself.
*/
void setup() {
    Serial.begin(115200);   // USB / CLI debug
    Serial1.begin(115200);  // Verso ESP32 Hub (Protobuf framing)

    Serial.println(F("\n[SmartVase] Platform Controller v" SMARTVASE_FW_VERSION " boot"));
    Serial.println(F("[SmartVase] CLI ready: type 'help'. For testing without Hub: 'standalone on'"));

    // Load config + stats from EEPROM (falling back to defaults if corrupted)
    persistence.loadConfig();
    persistence.loadStats();

    // Diagnose the reset cause using MCUSR captured in init3.
    if (mcusr_mirror & (1 << WDRF)) {
        persistence.getStats().watchdog_resets++;
        persistence.saveStats(true);
        comm.logEvent(Log_LogLevel_CRITICAL, "reboot_wdt", "watchdog_reset",
                      systemStatus.deviceId, persistence.getStats());
    } else {
        comm.logEvent(Log_LogLevel_INFO, "boot", "power_on_reset",
                      systemStatus.deviceId, persistence.getStats());
    }

    sensors.init();
    movement.init();
    pump.init();
    growLight.init();
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
        Serial.println(F("[SmartVase] RTC not detected on I2C 0x68"));
    } else if (sensors.rtcOscStopped()) {
        // Oscillatore fermo = ora non valida (modulo nuovo o batteria scarica).
        comm.logEvent(Log_LogLevel_WARN, "rtc_osf", "time_not_set",
                      systemStatus.deviceId, persistence.getStats());
        Serial.println(F("[SmartVase] RTC present but time NOT valid: use 'rtc set <epoch>'"));
    }

    comm.logEvent(Log_LogLevel_INFO, "system_boot", "platform_ready",
                  systemStatus.deviceId, persistence.getStats());

    // Allow hardware (especially ultrasonic sensors) to stabilize after a cold boot.
    // Without this, the HC-SR04 might return a false short pulse at power-on,
    // making the tank look full and triggering an immediate false irrigation.
    delay(1500);

    // From here on the watchdog is watching (resets after 4s stall).
    wdt_enable(WDTO_4S);

    Serial.println(F("[SmartVase] setup complete."));
}

// =================================================================
// Main loop (non-blocking)
// =================================================================

/*!
    @brief    Non-blocking main loop: CLI, communication, sensors, actuation, scheduler, health checks.
    @details  No blocking calls (no delay(), except performSoftReset
              which voluntarily terminates the firmware): each subsystem is
              "ticked" once per iteration and internally decides if it has work
              to do based on millis(). The order of steps is relevant:
              1. USB debug CLI; 2. serial RX from Hub (can execute commands);
              3. sensor sampling; 4. pump tick (auto-off/tank safety);
              5. movement state machine; 5b. grow lights (on only in
              IDLE with insufficient light); 6. periodic transmissions (telemetry/
              heartbeat/log) with independent interval scheduler; 7. persistence
              EEPROM (stats throttled, config deferred to idle serial);
              8. health checks (RAM, Hub deadman) with hysteresis to avoid oscillations;
              9. possible soft reset requested by command.
*/
void loop() {
    wdt_reset();
    const unsigned long now = millis();

    // 0) USB Serial debug CLI (Serial != Serial1)
    cli.tick(movement, sensors, pump, growLight, care, persistence, systemStatus);

    // 1) Serial RX (drain + potential command execution)
    comm.handleSerial(movement, persistence, sensors, pump, systemStatus);

    // 2) Sensor sampling (HC-SR04 + ADC round-robin)
    sensors.sampleSensors();

    // 3) Pompa: spegnimento automatico a fine timer + protezione tanica.
    //    If during watering the level (EMA filtered) drops below the
    //    threshold, or US4 stops giving valid readings, it stops immediately:
    //    never run the pump dry.
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

    // 4b) Autonomous care layer (L2, 1 Hz internally): light-budget accounting,
    //     care state machine, seeking/scan requests, dose/soak/verify watering.
    //     Passive unless enabled from the CLI (`care on`); every L0 safety
    //     (degraded mode, tank guard, pump caps) stays in charge below it.
    care.tick(movement, sensors, pump, persistence, comm, systemStatus);

    // 5b) Grow lights (UVA). With the care layer active they become the
    //     end-of-day budget top-up decided by the care state machine
    //     (CARE_TOP_UP); otherwise the legacy rule applies: on only if IDLE,
    //     ambient light insufficient AND within the simulated daylight window
    //     (see growLightWanted/withinDaylightWindow in SensorPolicy.h). Without
    //     a reliable RTC time they stay off, for fail-safe.
    if (care.isActive()) {
        growLight.force(care.growLightWanted() && !systemStatus.degradedModeActive);
    } else {
        growLight.update(movement.getTargetMode(), sensors.getLux(),
                         persistence.getConfig().light_threshold,
                         sensors.timeIsValid(), sensors.getEpoch());
    }

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
        // Autonomous-care KPIs (proto v4.1): daily counters owned by the Care
        // module, filled here because Sensors has no visibility on the care
        // layer (layer separation, see docs/Plant_Care_Design.md §2).
        {
            const DeviceConfig& cfg = persistence.getConfig();
            td.care_enabled = (cfg.care_enabled != 0);
            td.care_state   = care.stateCode();
            const float pct = care.budgetPct(cfg);
            td.light_budget_pct        = (pct <= 0.0f) ? 0 : (uint32_t)pct;
            td.relocations_today       = care.relocationsToday();
            td.water_doses_today       = care.dosesToday();
            td.growlight_minutes_today = care.growLightMinutesToday();
        }
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

    // 6) EEPROM stats (internally throttled)
    persistence.saveStats(false);

    // 6b) DEFERRED config save: writes to EEPROM only when the serial line
    //     to the Hub is idle, so the ~60 ms write doesn't land in the middle
    //     of a frame reception (risk of RX overflow / failed CRC).
    if (systemStatus.configSavePending && Serial1.available() == 0) {
        persistence.saveConfig(true);
        systemStatus.configSavePending = false;
    }

    // 7) Health checks
    const int freeBytes = freeRam();
    if (freeBytes < LOW_RAM_THRESHOLD_BYTES) {
        if (!systemStatus.lowMemoryDetected) {
            systemStatus.lowMemoryDetected = true;
            enterDegradedMode("low_sram");
        }
    } else if (systemStatus.lowMemoryDetected && freeBytes >= HIGH_RAM_THRESHOLD_BYTES) {
        systemStatus.lowMemoryDetected = false;
        // Recover only if no other reason keeps degraded mode active.
        if (!systemStatus.hubIsMissing) exitDegradedMode();
    }

    // Hub Deadman: suspended in standalone mode (bench test without Hub).
    if (systemStatus.standaloneMode) {
        if (systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = false;
            if (!systemStatus.lowMemoryDetected) exitDegradedMode();
        }
    } else if (millis() - comm.getLastHubMessageMs() > HUB_DEADMAN_TIMEOUT_MS) {
        if (!systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = true;
            enterDegradedMode("hub_missing");
        }
    } else if (systemStatus.hubIsMissing) {
        systemStatus.hubIsMissing = false;
        if (!systemStatus.lowMemoryDetected) exitDegradedMode();
    }

    // 8) Soft reset requested via command
    if (systemStatus.softResetRequested) {
        performSoftReset();
    }
}
