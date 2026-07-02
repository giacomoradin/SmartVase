/*!
    @file   smartvase_aliases.h

    @ingroup MegaMisc

    @brief  Alias verso i simboli protobuf nanopb + tipi C++ interni del Mega.

    @details Mappa i simboli generati da nanopb con prefisso `smartvase_`
             (definiti in `smartvase.pb.h`, rigenerato dal `.proto` canonico
             in `infra/smartvase-proto/`) ai nomi corti usati nel resto del
             firmware. Contiene inoltre i tipi C++ usati solo lato Mega
             (`DeviceConfig`, `CumulativeStats`, `CppMovementState`, `CppMode`)
             che NON fanno parte del protocollo Protobuf, tenuti
             deliberatamente separati dagli enum protobuf per non legare la
             logica interna allo schema di rete (vedi `cppMovementStateToProto`).

             Schema protobuf v4.0 — 2026-05-19 (refactor totale post-PIN map nuovo).

    @date   2025-10-20

    @author Giacomo Radin
*/

#ifndef SMARTVASE_ALIASES_H_INCLUDED
#define SMARTVASE_ALIASES_H_INCLUDED

#include "smartvase.pb.h"

/*!
    @addtogroup MegaMisc
    @{
*/

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

// --- Message Descriptors ---
#define WrapperMessage_fields smartvase_WrapperMessage_fields

// --- Initialization Macros ---
#define WrapperMessage_init_zero    smartvase_WrapperMessage_init_zero
#define Log_init_zero               smartvase_Log_init_zero
#define Command_init_zero           smartvase_Command_init_zero
#define CommandResponse_init_zero   smartvase_CommandResponse_init_zero
#define TelemetryFast_init_zero     smartvase_TelemetryFast_init_zero
#define TelemetryDeep_init_zero     smartvase_TelemetryDeep_init_zero
#define Heartbeat_init_zero         smartvase_Heartbeat_init_zero

// --- 'oneof' Field Tags ---
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

// --- Enum Values ---
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

/*!
    @struct DeviceConfig
    @brief  Configurazione del device persistita in EEPROM (dual-slot, vedi Persistence.h).
*/
typedef struct {
    uint32_t magic_number;       /**< Magic number per validare lo slot EEPROM. */
    uint16_t crc16;               /**< CRC16-CCITT della struct (vedi Crc16.h), validato al load. */
    uint16_t write_counter;       /**< Contatore di scrittura per il wear-leveling dual-slot. */
    uint8_t  motorCalibLeft;      /**< PWM di calibrazione motore sinistro, 0..255. */
    uint8_t  motorCalibRight;     /**< PWM di calibrazione motore destro, 0..255. */
    uint16_t avoid_reverse_ms;    /**< Durata retromarcia durante l'obstacle avoidance, in ms. */
    uint16_t avoid_turn_ms;       /**< Durata rotazione durante l'obstacle avoidance, in ms. */
    uint16_t soil_dry_threshold;  /**< Soglia ADC suolo sotto la quale il terreno è considerato 'dry'. */
    uint16_t light_threshold;     /**< Soglia ADC di luminosità per l'inversione light/shadow seeking. */
    uint16_t tank_empty_cm;       /**< Distanza US4→acqua (cm) oltre la quale la tanica è considerata vuota (pompa bloccata). */
    // --- Cura autonoma della pianta (layer L2, vedi CarePolicy.h / Care.cpp) ---
    uint8_t  care_enabled;            /**< 1 = cura autonoma attiva (default 0: si abilita esplicitamente con `care on`). */
    uint8_t  care_plant_kind;         /**< Preset pianta attivo (CarePlantKind: 0=shade, 1=medium, 2=sun). */
    uint16_t care_light_target_min;   /**< Target giornaliero di luce, in minuti di luce piena equivalente. */
    uint16_t care_lux_high_adc;       /**< ADC oltre il quale l'esposizione prolungata fa cercare ombra (1024 = mai). */
    uint16_t care_soil_wet_adc;       /**< ADC suolo al/oltre il quale il ciclo di irrigazione si ferma (isteresi con soil_dry_threshold). */
    uint16_t care_dose_ms;            /**< Durata di una singola dose di irrigazione (ms). */
    uint8_t  care_soak_min;           /**< Attesa di assorbimento tra due dosi (minuti). */
    uint8_t  care_max_doses;          /**< Tetto giornaliero di dosi (fail-safe forcella guasta). */
    uint8_t  care_max_reloc;          /**< Tetto giornaliero di ricollocazioni (anti-nomadismo). */
    uint8_t  care_growlight_max_min;  /**< Tetto giornaliero del top-up con luci UVA (minuti). */
} DeviceConfig;

/*!
    @struct CumulativeStats
    @brief  Statistiche cumulative di utilizzo persistite in EEPROM (dual-slot, vedi Persistence.h).
*/
typedef struct {
    uint32_t magic_number;                  /**< Magic number per validare lo slot EEPROM. */
    uint16_t crc16;                          /**< CRC16-CCITT della struct (vedi Crc16.h), validato al load. */
    uint16_t write_counter;                  /**< Contatore di scrittura per il wear-leveling dual-slot. */
    uint32_t total_motor_active_time_s;      /**< Tempo cumulativo di attività dei motori, in secondi. */
    uint32_t total_irrigations;              /**< Numero totale di irrigazioni eseguite. */
    uint32_t total_irrigation_duration_s;    /**< Durata cumulativa delle irrigazioni, in secondi. */
    uint32_t light_seeking_sessions;         /**< Numero di sessioni di ricerca luce (modalità LIGHT). */
    uint32_t shadow_seeking_sessions;        /**< Numero di sessioni di ricerca ombra (modalità SHADOW). */
    uint32_t obstacles_avoided;              /**< Numero di ostacoli evitati con successo. */
    uint32_t escape_attempts;                /**< Numero di tentativi di rilocazione anti-circling durante il seeking. */
    uint32_t stuck_events;                   /**< Numero di volte in cui il robot è entrato nello stato M_STUCK. */
    uint32_t watchdog_resets;                /**< Numero di reset causati dal watchdog timer. */
    uint32_t bme_read_errors;                /**< Numero di errori di lettura dal sensore BME680. */
    uint32_t log_overflows;                  /**< Numero di volte in cui la coda di log circolare ha scartato voci INFO per overflow. */
    uint32_t pb_decode_failures;             /**< Numero di frame seriali scartati per fallimento di decodifica protobuf o CRC. */
} CumulativeStats;

/*!
    @enum  CppMovementState
    @brief Stato della state machine di movimento, lato C++.

    @details Tenuto come tipo separato dall'enum protobuf `MovementState` per
             non legare la logica interna del main allo schema di rete; la
             conversione verso il tipo protobuf avviene esplicitamente con
             ::cppMovementStateToProto.
*/
typedef enum {
    CPP_M_IDLE,            /**< Robot fermo, nessun movimento target attivo. */
    CPP_M_MOVING,           /**< Movimento normale (seeking luce/ombra o nessuna ricerca). */
    CPP_M_AVOID_START,      /**< Ingresso nella sequenza di obstacle avoidance. */
    CPP_M_AVOID_REVERSING,  /**< Fase di retromarcia dell'obstacle avoidance. */
    CPP_M_AVOID_TURNING,    /**< Fase di rotazione dell'obstacle avoidance. */
    CPP_M_STUCK,            /**< Robot bloccato, in attesa di backoff prima di un nuovo tentativo. */
    CPP_M_SCAN_ROTATE,      /**< Light scan: rotazione sul posto campionando l'LDR per settori (vedi startLightScan). */
    CPP_M_SCAN_ALIGN        /**< Light scan: seconda rotazione fino al settore migliore individuato. */
} CppMovementState;

/*!
    @enum  CppMode
    @brief Modalità operative del robot (impostate da CLI o da comando Hub `setMode`).
*/
typedef enum {
    CPP_IDLE,    /**< Nessuna ricerca attiva: il robot resta fermo se non in movimento manuale. */
    CPP_LIGHT,   /**< Ricerca della luce (light seeking). */
    CPP_SHADOW   /**< Ricerca dell'ombra (shadow seeking). */
} CppMode;

/*!
    @brief    Converte uno stato di movimento C++ nel corrispondente enum protobuf.

    @param[in] s Stato di movimento lato C++.

    @return   Il valore ::MovementState equivalente da inviare in `TelemetryFast`.

    @note     `CPP_M_IDLE` e qualunque valore non riconosciuto mappano su `MS_IDLE` (default fail-safe).
*/
static inline MovementState cppMovementStateToProto(CppMovementState s) {
    switch (s) {
        case CPP_M_MOVING:          return MS_MOVING;
        // Gli stati interni di light scan sono una forma di movimento attivo:
        // verso lo schema di rete (v4.0, senza stati scan) si riportano come MOVING.
        case CPP_M_SCAN_ROTATE:     return MS_MOVING;
        case CPP_M_SCAN_ALIGN:      return MS_MOVING;
        case CPP_M_AVOID_START:     return MS_AVOID_START;
        case CPP_M_AVOID_REVERSING: return MS_AVOID_REVERSING;
        case CPP_M_AVOID_TURNING:   return MS_AVOID_TURNING;
        case CPP_M_STUCK:           return MS_STUCK;
        case CPP_M_IDLE:
        default:                    return MS_IDLE;
    }
}

/*! @} */ // MegaMisc

#endif // SMARTVASE_ALIASES_H_INCLUDED
