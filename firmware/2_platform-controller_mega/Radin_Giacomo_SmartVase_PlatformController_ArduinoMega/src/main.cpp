/*
 * =================================================================
 * SmartVase - Platform Controller (Arduino Mega)
 * Versione: 4.0 (Refactored)
 * =================================================================
 */

// =================================================================
// 1. LIBRARIES & HEADERS
// =================================================================
#include "Movement.h"
#include "Sensors.h"
#include "Communication.h"
#include "Persistence.h"

#include <avr/wdt.h>

// =================================================================
// 2. GLOBAL OBJECTS
// =================================================================
Movement movement;
Sensors sensors;
Communication comm;
Persistence persistence;

// =================================================================
// 3. GLOBAL VARIABLES & DEFINITIONS
// =================================================================
struct SystemStatus {
    bool bmeSensorError; bool lowMemoryDetected; bool logQueueOverflow; bool degradedModeActive; bool hubIsMissing; char degradedReason[32];
};
SystemStatus systemStatus = {false, false, false, false, true, ""};

uint8_t mcusr_mirror __attribute__ ((section (".noinit")));
unsigned long lastHubMessageTime = 0;

// =================================================================
// 4. UTILITY FUNCTIONS
// =================================================================
int freeRam() { extern int __heap_start, *__brkval; int v; return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) &__brkval); }
void wdt_init(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdt_init(void) { mcusr_mirror = MCUSR; MCUSR = 0; wdt_disable(); }

void enterDegradedMode(const char* reason) {
    if (systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = true;
    strncpy(systemStatus.degradedReason, reason, sizeof(systemStatus.degradedReason) - 1);
    systemStatus.degradedReason[sizeof(systemStatus.degradedReason)-1] = '\0';
    movement.stopMotors();
    movement.setTargetMode(IDLE);
    comm.logEvent(Log_LogLevel_CRITICAL, "degraded_mode", reason);
    persistence.saveStats(true);
}

void exitDegradedMode() {
    if (!systemStatus.degradedModeActive) return;
    systemStatus.degradedModeActive = false;
    systemStatus.degradedReason[0] = '\0';
    comm.logEvent(Log_LogLevel_INFO, "degraded_exit", "Exiting degraded mode");
}

void softReset() {
    comm.logEvent(Log_LogLevel_WARN, "soft_reset", "Initiating software reset");
    delay(100);
    wdt_enable(WDTO_15MS);
    while (true);
}

// =================================================================
// 5. SETUP & MAIN LOOP
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println(F("\nInizializzazione Platform Controller v4.0..."));
    
    Serial1.begin(115200);

    mcusr_mirror = MCUSR;
    MCUSR = 0;
    wdt_disable();

    persistence.loadConfig();
    persistence.loadStats();

    if (mcusr_mirror & (1 << WDRF)) {
        persistence.getStats().watchdog_resets++;
        persistence.saveStats(true);
        comm.logEvent(Log_LogLevel_CRITICAL, "reboot_wdt", "Watchdog reset");
    } else {
        comm.logEvent(Log_LogLevel_INFO, "boot", "Power-on reset");
    }
    wdt_enable(WDTO_4S);

    sensors.init();
    movement.init();
    
    if (!sensors.getBMEStatus()) {
        systemStatus.bmeSensorError = true;
        comm.logEvent(Log_LogLevel_ERROR, "sensor_init_fail", "BME680");
    }
    
    comm.logEvent(Log_LogLevel_INFO, "system_boot", "Platform Controller ready");
    Serial.println(F("Setup completo."));
}

void loop() {
    wdt_reset();
    unsigned long currentMillis = millis();

    comm.handleSerial(movement, persistence);
    sensors.sampleSensors();
    movement.handleMovementSM(sensors.getFrontDistance() < 20, sensors.getLeftDistance() < 20, sensors.getRightDistance() < 20, sensors.getLux(), persistence.getConfig(), persistence.getStats(), systemStatus.degradedModeActive);

    // Task scheduling
    // ...
    
    persistence.saveStats(false);

    if (freeRam() < 800) {
        enterDegradedMode("Low SRAM");
    }
    
    if (currentMillis - lastHubMessageTime > 120000) {
        if (!systemStatus.hubIsMissing) {
            systemStatus.hubIsMissing = true;
            enterDegradedMode("Hub Missing");
        }
    }
}
