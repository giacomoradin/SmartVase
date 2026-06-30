/*!
    @file   Communication.h

    @ingroup MegaComm

    @brief  Gestore della comunicazione seriale (Protobuf framato) tra Mega e Hub.

    @date   2026-04-29

    @author Giacomo Radin
*/

#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <Arduino.h>
extern "C" {
  #include "pb_encode.h"
  #include "pb_decode.h"
  #include "smartvase.pb.h"
}
#include "smartvase_aliases.h"

// Forward declarations per evitare include circolari.
class Movement;
class Persistence;
class Sensors;
class Pump;
struct SystemStatus;

/*!
    @addtogroup MegaComm
    @{
*/

/*!
    @def    PROTOBUF_BUFFER_SIZE
    @brief  Dimensione massima del buffer di trasmissione e ricezione dei messaggi Protobuf framati (byte).
*/
#define PROTOBUF_BUFFER_SIZE 256

/*!
    @class Communication
    @brief Gestore della comunicazione seriale (Protobuf framato) tra Arduino Mega e ESP32 Hub.

    @details Implementa la decodifica non bloccante a stati dei frame ricevuti su `Serial1`
             (framing `SOF | len_hi | len_lo | payload | crc16_hi | crc16_lo`, vedi Crc16.h),
             l'estrazione del payload Protobuf, l'esecuzione e deduplicazione idempotente dei
             comandi (via `_lastExecutedCmdId`), e l'invio dei frame strutturati di telemetria,
             log e ACK verso l'Hub. Applica inoltre le policy di sicurezza definite in
             `CommandPolicy.h` (clamp/rate-limit) come difesa indipendente da quella già
             applicata lato Hub.
*/
class Communication {
public:
    /**
     * @brief Costruttore della classe Communication.
     */
    Communication();

    /**
     * @brief Inizializza l'interfaccia seriale Serial1 a 115200 baud.
     */
    void init();

    /**
     * @brief Legge e processa i dati in arrivo sulla porta seriale dal modulo Hub.
     * 
     * Da chiamare ciclicamente nel loop principale. Gestisce il parsing del frame e attiva
     * l'esecuzione dei comandi estratti dal payload.
     * 
     * @param movement Modulo movimento per pilotare la modalità o testare comandi.
     * @param persistence Modulo di persistenza per salvare impostazioni e statistiche.
     * @param sensors Modulo sensori per leggere igrometro, lux, distanze.
     * @param pump Modulo pompa per irrigare.
     * @param sys Stato complessivo del sistema per monitorare degraded mode.
     */
    void handleSerial(Movement& movement, Persistence& persistence,
                      Sensors& sensors, Pump& pump, SystemStatus& sys);

    /**
     * @brief Invia un messaggio Protobuf (WrapperMessage) framato sulla seriale.
     * @param message Messaggio wrapper Protobuf da inviare.
     */
    void sendProtobufMessage(const WrapperMessage& message);

    /**
     * @brief Invia le telemetrie rapide (TelemetryFast) tramite seriale.
     */
    void sendFastTelemetry(const TelemetryFast& tf);

    /**
     * @brief Invia le telemetrie approfondite (TelemetryDeep) tramite seriale.
     */
    void sendDeepTelemetry(const TelemetryDeep& td);

    /**
     * @brief Invia l'heartbeat periodico all'Hub.
     * @param uptime_s Tempo di funzionamento in secondi.
     * @param isDegraded Flag che indica se il sistema è in degraded mode.
     * @param deviceId ID univoco del dispositivo.
     */
    void sendHeartbeat(uint32_t uptime_s, bool isDegraded, const char* deviceId);

    /**
     * @brief Invia la risposta ad un comando ricevuto (ACK).
     */
    void sendCommandResponse(CommandResponse_Status status, const char* detail,
                             int32_t value, uint32_t cmd_id, uint32_t exec_time_ms);

    /**
     * @brief Accoda un evento di log per l'invio.
     * 
     * I log vengono scritti in una coda circolare per non bloccare l'esecuzione della CPU Mega.
     */
    void logEvent(Log_LogLevel level, const char* event, const char* detail,
                  const char* deviceId, CumulativeStats& stats);

    /**
     * @brief Estrae un log dalla coda e lo trasmette all'Hub.
     * 
     * Chiamato periodicamente dallo scheduler non bloccante.
     */
    void drainLogQueue(const char* deviceId);

    /**
     * @brief Restituisce il timestamp in ms dell'ultimo messaggio valido ricevuto dall'Hub.
     * @return unsigned long Millisecondi da millis().
     */
    unsigned long getLastHubMessageMs() const { return last_hub_message_ms; }

private:
    /**
     * @brief Incapsula un payload Protobuf nel frame seriale SOF/len/payload/CRC ed esegue l'invio.
     */
    void sendFramedMessage(const uint8_t* payload, uint16_t len);

    /**
     * @brief Esegue i comandi ricevuti dal modulo Hub, applicando policy di clamp e deduplicazione.
     */
    void executeCommand(const WrapperMessage& message,
                        Movement& movement, Persistence& persistence,
                        Sensors& sensors, Pump& pump, SystemStatus& sys);

    uint8_t protobuf_tx_buffer[PROTOBUF_BUFFER_SIZE];
    uint8_t protobuf_rx_buffer[PROTOBUF_BUFFER_SIZE];

    unsigned long last_hub_message_ms;  /**< Millis dell'ultimo messaggio ricevuto */
    unsigned long last_water_ms;         /**< Timestamp dell'ultima irrigazione per prevenire over-watering accidental */
    uint32_t      _lastExecutedCmdId;    /**< ID dell'ultimo comando eseguito con successo (per idempotenza) */
};

/*! @} */ // MegaComm

#endif // COMMUNICATION_H
