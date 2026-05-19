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

#define PROTOBUF_BUFFER_SIZE 256

class Communication {
public:
    Communication();
    void init();

    // Pump il main loop: drena RX seriale, esegue eventuali comandi.
    // Sensors/Pump/SystemStatus servono per rispondere ad alcuni comandi
    // (ReadSoilCommand, WaterCommand, RequestDiagnostics, ...).
    void handleSerial(Movement& movement, Persistence& persistence,
                      Sensors& sensors, Pump& pump, SystemStatus& sys);

    // Trasmissione messaggi Protobuf framati.
    void sendProtobufMessage(const WrapperMessage& message);
    void sendFastTelemetry(const TelemetryFast& tf);
    void sendDeepTelemetry(const TelemetryDeep& td);
    void sendHeartbeat(uint32_t uptime_s, bool isDegraded, const char* deviceId);
    void sendCommandResponse(CommandResponse_Status status, const char* detail,
                             int32_t value, uint32_t cmd_id, uint32_t exec_time_ms);

    // Log strutturato: enqueue + drain.
    void logEvent(Log_LogLevel level, const char* event, const char* detail,
                  const char* deviceId, CumulativeStats& stats);
    // Invia un log (se presente in coda) — chiamato dallo scheduler del main.
    void drainLogQueue(const char* deviceId);

    // Restituisce l'epoch in ms del piu' recente messaggio valido ricevuto.
    // Usato per il deadman switch Hub-Missing.
    unsigned long getLastHubMessageMs() const { return last_hub_message_ms; }

private:
    void sendFramedMessage(const uint8_t* payload, uint16_t len);
    void executeCommand(const WrapperMessage& message,
                        Movement& movement, Persistence& persistence,
                        Sensors& sensors, Pump& pump, SystemStatus& sys);

    uint8_t protobuf_tx_buffer[PROTOBUF_BUFFER_SIZE];
    uint8_t protobuf_rx_buffer[PROTOBUF_BUFFER_SIZE];

    unsigned long last_hub_message_ms;
};

#endif // COMMUNICATION_H
