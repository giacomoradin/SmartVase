#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <Arduino.h>
extern "C" {
  #include "pb_encode.h"
  #include "pb_decode.h"
  #include "smartvase.pb.h"
}
#include "smartvase_aliases.h"

// Forward declarations
class Movement;
class Persistence;

#define PROTOBUF_BUFFER_SIZE 256

class Communication {
public:
    Communication();
    void handleSerial(Movement& movement, Persistence& persistence);
    void sendProtobufMessage(const WrapperMessage& message);
    void logEvent(Log_LogLevel level, const char* event, const char* detail);
    void sendLog();
    void sendHeartbeat();
    void sendFastTelemetry(const TelemetryFast& tf);
    void sendDeepTelemetry(const TelemetryDeep& td);
    void sendCommandResponse(CommandResponse_Status status, const char* detail, uint32_t cmd_id, uint32_t exec_time);


private:
    void sendFramedMessage(const uint8_t* payload, uint16_t len);
    void executeCommand(const WrapperMessage& message, Movement& movement, Persistence& persistence);
    uint16_t crc16(const uint8_t* data, size_t length);

    uint8_t protobuf_tx_buffer[PROTOBUF_BUFFER_SIZE];
    uint8_t protobuf_rx_buffer[PROTOBUF_BUFFER_SIZE];
};

#endif // COMMUNICATION_H