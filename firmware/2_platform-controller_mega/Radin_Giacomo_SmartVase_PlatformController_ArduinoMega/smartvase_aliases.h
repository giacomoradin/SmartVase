/*
 * =================================================================
 * SmartVase - Protobuf Alias Definitions
 * =================================================================
 * Questo file mappa i simboli generati da nanopb (con prefisso 'smartvase_')
 * ai nomi più brevi utilizzati nel firmware, risolvendo gli errori di
 * compilazione dell'Arduino IDE.
 *
 * Includere questo file subito dopo il blocco extern "C" con gli
 * header di nanopb.
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

// --- Alias per i Descrittori dei Messaggi ---
#define WrapperMessage_fields smartvase_WrapperMessage_fields
// --- Alias per le Macro di Inizializzazione ---
#define WrapperMessage_init_zero    smartvase_WrapperMessage_init_zero
#define Log_init_zero               smartvase_Log_init_zero
#define Command_init_zero           smartvase_Command_init_zero
#define CommandResponse_init_zero   smartvase_CommandResponse_init_zero
#define TelemetryFast_init_zero     smartvase_TelemetryFast_init_zero
#define TelemetryDeep_init_zero     smartvase_TelemetryDeep_init_zero
#define Heartbeat_init_zero         smartvase_Heartbeat_init_zero
// --- Alias per i Tag dei Campi 'oneof' ---
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

// --- Alias per i Valori degli Enum ---
#define Log_LogLevel_INFO      smartvase_Log_LogLevel_INFO
#define Log_LogLevel_WARN      smartvase_Log_LogLevel_WARN
#define Log_LogLevel_ERROR     smartvase_Log_LogLevel_ERROR
#define Log_LogLevel_CRITICAL  smartvase_Log_LogLevel_CRITICAL

#define CommandResponse_Status_OK    smartvase_CommandResponse_Status_OK
#define CommandResponse_Status_ERROR smartvase_CommandResponse_Status_ERROR

#define IDLE   smartvase_SetModeCommand_Mode_IDLE
#define LIGHT  smartvase_SetModeCommand_Mode_LIGHT
#define SHADOW smartvase_SetModeCommand_Mode_SHADOW

#define M_IDLE            smartvase_MovementState_M_IDLE
#define M_MOVING          smartvase_MovementState_M_MOVING
#define M_AVOID_START     smartvase_MovementState_M_AVOID_START
#define M_AVOID_REVERSING smartvase_MovementState_M_AVOID_REVERSING
#define M_AVOID_TURNING   smartvase_MovementState_M_AVOID_TURNING
#define M_STUCK           smartvase_MovementState_M_STUCK

#endif // SMARTVASE_ALIASES_H_INCLUDED
