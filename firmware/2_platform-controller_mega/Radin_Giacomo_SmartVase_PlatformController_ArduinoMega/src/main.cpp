/*!
    @file   main.cpp

    @ingroup MegaCore

    @brief  Setup/loop del Platform Controller (Arduino Mega) — "The Brawn".

    @details Versione 5.2:
             - 6 sensori HC-SR04 con pin del PIN map autoritativo (driver locale Ultrasonic)
             - Forcella umidità suolo su A0, LDR su A1
             - Pompa irrigazione via relè D10 con protezione tanica vuota (US4)
             - Luci di coltivazione (UVA) via relè D11, accese automaticamente in IDLE con luce insufficiente
             - RTC DS3232 (I2C 0x68) per timestamp epoch nei messaggi
             - WDT 4s, doppio slot EEPROM con CRC16 (wear-leveling), log queue
             - Comunicazione Serial1 Protobuf+framing verso ESP32 Hub
             - Scheduler non bloccante per telemetria/heartbeat/log
             - Modalità standalone (CLI) per test a banco senza Hub

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
    @brief    Stima la SRAM libera residua (heap libero rispetto allo stack).
    @details  Usata dagli health check del loop e dai comandi CLI `status`/`diag`
              per rilevare condizioni di RAM bassa prima che causino corruzione
              dello stack (AVR non ha protezione di memoria).
    @return   Byte liberi stimati tra la fine dello heap (`__brkval`/`__heap_start`)
              e lo stack pointer corrente.
*/
int freeRam() {
    extern int __heap_start, *__brkval;
    int v;
    return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) &__brkval);
}

/*!
    @brief    Disabilita il watchdog subito dopo il reset hardware, prima di `setup()`.
    @details  Gira nella sezione `.init3` (eseguita prima dell'inizializzazione delle
              variabili globali), quindi prima ancora di `main()`. Necessario perché
              su AVR il WDT resta attivo dopo un reset causato dal WDT stesso: senza
              questa disabilitazione immediata, un boot lento (es. inizializzazione
              sensori) potrebbe essere interrotto da un secondo reset prima che
              `setup()` possa richiamare `wdt_enable()` con il timeout definitivo.
              Salva inoltre `MCUSR` in `mcusr_mirror` (sezione `.noinit`, sopravvive
              al reset) per permettere a `setup()` di distinguere un reset da
              watchdog da un power-on normale.
*/
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) {
    mcusr_mirror = MCUSR;
    MCUSR = 0;
    wdt_disable();
}

/*!
    @brief    Entra in modalità degradata, disabilitando motori e pompa.
    @details  Idempotente: se il sistema è già in degraded mode non fa nulla (evita
              di rilanciare il log/salvataggio statistiche ad ogni iterazione del
              loop mentre la condizione persiste). Registra il motivo, ferma
              immediatamente l'attuazione fisica (motori e pompa), logga un evento
              CRITICAL verso l'Hub e forza il salvataggio delle statistiche.
    @param[in] reason Stringa breve che identifica la causa (es. "low_sram", "hub_missing"),
                       copiata in `systemStatus.degradedReason`.
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
    @brief    Esce dalla modalità degradata e logga il recupero.
    @details  Va chiamata solo quando NESSuna delle condizioni di degraded mode è
              più attiva (RAM bassa, Hub assente, ecc.): i chiamanti nel `loop()`
              verificano già le altre cause prima di invocarla. Idempotente: se il
              sistema non è in degraded mode non fa nulla.
*/
void exitDegradedMode() {
    if (!systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = false;
    systemStatus.degradedReason[0] = '\0';
    comm.logEvent(Log_LogLevel_INFO, "degraded_exit", "recovered",
                  systemStatus.deviceId, persistence.getStats());
}

/*!
    @brief    Esegue un reset software via watchdog, su richiesta di un comando.
    @details  Salva le statistiche, logga l'evento, poi forza un reset volontario
              armando il WDT con un timeout brevissimo (15 ms) ed entrando in un
              loop infinito di attesa: non c'è un modo "pulito" di fare un reset
              software su AVR, quindi si sfrutta deliberatamente il watchdog.
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
    @brief    Inizializzazione del firmware: seriali, EEPROM, sensori, attuatori, watchdog.
    @details  Ordine rilevante: carica config/stats da EEPROM prima di diagnosticare
              il motivo del reset (serve per poter incrementare/salvare
              `watchdog_resets`), poi abilita il WDT (4 s) solo dopo aver completato
              le inizializzazioni potenzialmente lente, per non rischiare un reset
              prematuro durante il boot stesso.
*/
void setup() {
    Serial.begin(115200);   // USB / CLI debug
    Serial1.begin(115200);  // Verso ESP32 Hub (Protobuf framing)

    Serial.println(F("\n[SmartVase] Platform Controller v" SMARTVASE_FW_VERSION " boot"));
    Serial.println(F("[SmartVase] CLI pronta: digita 'help'. Per test senza Hub: 'standalone on'"));

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
        Serial.println(F("[SmartVase] RTC non rilevato su I2C 0x68"));
    } else if (sensors.rtcOscStopped()) {
        // Oscillatore fermo = ora non valida (modulo nuovo o batteria scarica).
        comm.logEvent(Log_LogLevel_WARN, "rtc_osf", "time_not_set",
                      systemStatus.deviceId, persistence.getStats());
        Serial.println(F("[SmartVase] RTC presente ma ora NON valida: usa 'rtc set <epoch>'"));
    }

    comm.logEvent(Log_LogLevel_INFO, "system_boot", "platform_ready",
                  systemStatus.deviceId, persistence.getStats());

    // Da qui in poi il watchdog vigila (resetta dopo 4s di stallo).
    wdt_enable(WDTO_4S);

    Serial.println(F("[SmartVase] setup complete."));
}

// =================================================================
// Main loop (non bloccante)
// =================================================================

/*!
    @brief    Ciclo principale non bloccante: CLI, comunicazione, sensori, attuazione, scheduler, health check.
    @details  Nessuna chiamata bloccante (niente `delay()`, salvo `performSoftReset`
              che termina volontariamente il firmware): ogni sotto-sistema viene
              "tickato" una volta per iterazione e decide internamente se ha lavoro
              da fare in base a `millis()`. L'ordine dei passi è rilevante:
              1. CLI debug USB; 2. RX seriale dall'Hub (può eseguire comandi);
              3. campionamento sensori; 4. tick pompa (autospegnimento/safety tanica);
              5. state machine movimento; 5b. luci di coltivazione (accese solo in
              IDLE con luce insufficiente); 6. trasmissioni periodiche (telemetria/
              heartbeat/log) con scheduler a intervalli indipendenti; 7. persistenza
              EEPROM (stats throttled, config deferita a seriale inattiva);
              8. health check (RAM, deadman Hub) con isteresi per evitare oscillazioni;
              9. eventuale soft reset richiesto da comando.
*/
void loop() {
    wdt_reset();
    const unsigned long now = millis();

    // 0) CLI debug su USB Serial (Serial != Serial1)
    cli.tick(movement, sensors, pump, growLight, persistence, systemStatus);

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

    // 5b) Grow lights (UVA): turned on only if the robot is IDLE, the ambient
    //     light is insufficient AND we are within the simulated daylight window
    //     (see growLightWanted/withinDaylightWindow in SensorPolicy.h). Without
    //     a reliable RTC time they stay off, for fail-safe.
    growLight.update(movement.getTargetMode(), sensors.getLux(),
                     persistence.getConfig().light_threshold,
                     sensors.timeIsValid(), sensors.getEpoch());

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
        // Recover solo se nessun altro motivo tiene attivo degraded mode.
        if (!systemStatus.hubIsMissing) exitDegradedMode();
    }

    // Deadman Hub: sospeso in modalita' standalone (test a banco senza Hub).
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

    // 8) Soft reset richiesto via comando
    if (systemStatus.softResetRequested) {
        performSoftReset();
    }
}
