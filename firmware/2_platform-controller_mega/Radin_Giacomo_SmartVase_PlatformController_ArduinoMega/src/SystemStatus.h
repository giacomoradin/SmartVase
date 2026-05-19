#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <Arduino.h>

#define DEVICE_ID_MAX_LEN 16
#define DEGRADED_REASON_MAX_LEN 32

// Stato condiviso del sistema (passato per riferimento ai moduli che
// devono leggerlo o aggiornarlo: Communication, main loop, ecc.).
struct SystemStatus {
    bool bmeSensorError;
    bool lowMemoryDetected;
    bool degradedModeActive;
    bool hubIsMissing;
    bool softResetRequested;
    char degradedReason[DEGRADED_REASON_MAX_LEN];
    char deviceId[DEVICE_ID_MAX_LEN];
};

#endif // SYSTEM_STATUS_H
