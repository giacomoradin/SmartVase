/*!
    @file   main.cpp

    @ingroup MegaCore

    @brief  Setup/loop del Platform Controller (Arduino Mega) — "The Brawn".

    @details Versione 5.2:
             - 6 sensori HC-SR04 con pin del PIN map autoritativo (driver locale Ultrasonic)
             - Forcella umidità suolo su A0, LDR su A1
             - Pompa irrigazione via relè D10 con protezione tanica vuota (US4)
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
SystemStatus  systemStatus = {false, false, false, true, false, false, false, "", "MEGA_01"};

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

/*!
    @brief    Ciclo principale non bloccante: CLI, comunicazione, sensori, attuazione, scheduler, health check.
    @details  Nessuna chiamata bloccante (niente `delay()`, salvo `performSoftReset`
              che termina volontariamente il firmware): ogni sotto-sistema viene
              "tickato" una volta per iterazione e decide internamente se ha lavoro
              da fare in base a `millis()`. L'ordine dei passi è rilevante:
              1. CLI debug USB; 2. RX seriale dall'Hub (può eseguire comandi);
              3. campionamento sensori; 4. tick pompa (autospegnimento/safety tanica);
              5. state machine movimento; 6. trasmissioni periodiche (telemetria/
              heartbeat/log) con scheduler a intervalli indipendenti; 7. persistenza
              EEPROM (stats throttled, config deferita a seriale inattiva);
              8. health check (RAM, deadman Hub) con isteresi per evitare oscillazioni;
              9. eventuale soft reset richiesto da comando.
*/
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

    // 6b) Salvataggio config DEFERITO: scrive in EEPROM solo quando la seriale
    //     verso l'Hub e' a riposo, cosi' i ~60 ms di scrittura non cadono in
    //     mezzo alla ricezione di un frame (rischio overflow RX / CRC falliti).
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
