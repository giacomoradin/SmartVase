/*
 * =================================================================
 * SmartVase - Protobuf Alias Definitions + Tipi C++ interni
 * =================================================================
 * Mappa i simboli nanopb generati con prefisso 'smartvase_' ai nomi
 * corti usati nel firmware. Include anche struct/enum C++ usati solo
 * lato Mega e non parte del protocollo Protobuf.
 *
 * Schema v4.0 — 2026-05-19 (refactor totale post-PIN map nuovo).
 */

#ifndef SMARTVASE_ALIASES_H_INCLUDED
#define SMARTVASE_ALIASES_H_INCLUDED

#include "smartvase.pb.h"

// --- Alias per Tipi (Structs & Enums) ---
typedef smartvase_WrapperMessage         WrapperMessage;
typedef smartvase_Log_LogLevel           Log_LogLevel;
typedef smartvase_Command                Command;
typedef smartvase_CommandResponse_Status CommandResponse_Status;
typedef smartvase_MovementState          MovementState;
typedef smartvase_SetModeCommand_Mode    SetModeCommand_Mode;
typedef smartvase_TelemetryFast          TelemetryFast;
typedef smartvase_TelemetryDeep          TelemetryDeep;
typedef smartvase_Log                    Log;
typedef smartvase_Heartbeat              Heartbeat;
typedef smartvase_CommandResponse        CommandResponse;

// --- Descrittori dei Messaggi ---
#define WrapperMessage_fields smartvase_WrapperMessage_fields

// --- Macro di Inizializzazione ---
#define WrapperMessage_init_zero    smartvase_WrapperMessage_init_zero
#define Log_init_zero               smartvase_Log_init_zero
#define Command_init_zero           smartvase_Command_init_zero
#define CommandResponse_init_zero   smartvase_CommandResponse_init_zero
#define TelemetryFast_init_zero     smartvase_TelemetryFast_init_zero
#define TelemetryDeep_init_zero     smartvase_TelemetryDeep_init_zero
#define Heartbeat_init_zero         smartvase_Heartbeat_init_zero

// --- Tag dei Campi 'oneof' ---
#define WrapperMessage_telemetry_fast_tag   smartvase_WrapperMessage_telemetry_fast_tag
#define WrapperMessage_telemetry_deep_tag   smartvase_WrapperMessage_telemetry_deep_tag
#define WrapperMessage_log_tag              smartvase_WrapperMessage_log_tag
#define WrapperMessage_heartbeat_tag        smartvase_WrapperMessage_heartbeat_tag
#define WrapperMessage_command_tag          smartvase_WrapperMessage_command_tag
#define WrapperMessage_command_response_tag smartvase_WrapperMessage_command_response_tag

#define Command_water_tag                 smartvase_Command_water_tag
#define Command_set_mode_tag              smartvase_Command_set_mode_tag
#define Command_stop_tag                  smartvase_Command_stop_tag
#define Command_request_diagnostics_tag   smartvase_Command_request_diagnostics_tag
#define Command_set_motion_params_tag     smartvase_Command_set_motion_params_tag
#define Command_read_soil_tag             smartvase_Command_read_soil_tag
#define Command_soft_reset_tag            smartvase_Command_soft_reset_tag

// --- Valori degli Enum ---
#define Log_LogLevel_INFO      smartvase_Log_LogLevel_INFO
#define Log_LogLevel_WARN      smartvase_Log_LogLevel_WARN
#define Log_LogLevel_ERROR     smartvase_Log_LogLevel_ERROR
#define Log_LogLevel_CRITICAL  smartvase_Log_LogLevel_CRITICAL

#define CommandResponse_Status_OK    smartvase_CommandResponse_Status_OK
#define CommandResponse_Status_ERROR smartvase_CommandResponse_Status_ERROR

#define SetModeCommand_Mode_IDLE   smartvase_SetModeCommand_Mode_IDLE
#define SetModeCommand_Mode_LIGHT  smartvase_SetModeCommand_Mode_LIGHT
#define SetModeCommand_Mode_SHADOW smartvase_SetModeCommand_Mode_SHADOW

#define MS_IDLE            smartvase_MovementState_M_IDLE
#define MS_MOVING          smartvase_MovementState_M_MOVING
#define MS_AVOID_START     smartvase_MovementState_M_AVOID_START
#define MS_AVOID_REVERSING smartvase_MovementState_M_AVOID_REVERSING
#define MS_AVOID_TURNING   smartvase_MovementState_M_AVOID_TURNING
#define MS_STUCK           smartvase_MovementState_M_STUCK

// =================================================================
// Tipi C++ interni (non Protobuf)
// =================================================================

// Configurazione device persistita in EEPROM
typedef struct {
    uint32_t magic_number;
    uint16_t crc16;
    uint8_t  motorCalibLeft;     // PWM 0..255
    uint8_t  motorCalibRight;    // PWM 0..255
    uint16_t avoid_reverse_ms;   // durata retromarcia in avoidance
    uint16_t avoid_turn_ms;      // durata rotazione in avoidance
    uint16_t soil_dry_threshold; // soglia ADC sotto la quale 'dry'
    uint16_t light_threshold;    // soglia lux per inversione light/shadow
} DeviceConfig;

// Statistiche cumulative persistite in EEPROM
typedef struct {
    uint32_t magic_number;
    uint16_t crc16;
    uint32_t total_motor_active_time_s;
    uint32_t total_irrigations;
    uint32_t total_irrigation_duration_s;
    uint32_t light_seeking_sessions;
    uint32_t shadow_seeking_sessions;
    uint32_t obstacles_avoided;
    uint32_t escape_attempts;
    uint32_t stuck_events;
    uint32_t watchdog_resets;
    uint32_t bme_read_errors;
    uint32_t log_overflows;
    uint32_t pb_decode_failures;
} CumulativeStats;

// Stato della macchina a stati del movimento (lato C++).
// Tenuto in tipo separato per non legare il main al tipo Protobuf.
typedef enum {
    CPP_M_IDLE,
    CPP_M_MOVING,
    CPP_M_AVOID_START,
    CPP_M_AVOID_REVERSING,
    CPP_M_AVOID_TURNING,
    CPP_M_STUCK
} CppMovementState;

// Modalita' operative
typedef enum {
    CPP_IDLE,
    CPP_LIGHT,
    CPP_SHADOW
} CppMode;

// Mappatura stati C++ <-> enum Protobuf MovementState
static inline MovementState cppMovementStateToProto(CppMovementState s) {
    switch (s) {
        case CPP_M_MOVING:          return MS_MOVING;
        case CPP_M_AVOID_START:     return MS_AVOID_START;
        case CPP_M_AVOID_REVERSING: return MS_AVOID_REVERSING;
        case CPP_M_AVOID_TURNING:   return MS_AVOID_TURNING;
        case CPP_M_STUCK:           return MS_STUCK;
        case CPP_M_IDLE:
        default:                    return MS_IDLE;
    }
}

#endif // SMARTVASE_ALIASES_H_INCLUDED
