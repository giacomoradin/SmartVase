#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

#include <Arduino.h>

// Versione firmware: loggata al boot e mostrata dalla CLI (`version`).
#define SMARTVASE_FW_VERSION "5.1.0"

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
    // Modalita' banco di prova (CLI `standalone on`): sospende il deadman
    // dell'Hub cosi' il Mega non entra in degraded mode quando si testa
    // da solo via USB. Non persistita: torna off a ogni reset.
    bool standaloneMode;
    char degradedReason[DEGRADED_REASON_MAX_LEN];
    char deviceId[DEVICE_ID_MAX_LEN];
};

#endif // SYSTEM_STATUS_H
