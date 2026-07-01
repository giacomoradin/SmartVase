/*!
    @file   Communication.h

    @ingroup MegaComm

    @brief  Serial communication manager (framed Protobuf) between the Mega and the Hub.

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

// Forward declarations to avoid circular includes.
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
    @brief  Maximum size of the transmit and receive buffer for framed Protobuf messages (bytes).
*/
#define PROTOBUF_BUFFER_SIZE 256

/*!
    @class Communication
    @brief Serial communication manager (framed Protobuf) between the Arduino Mega and the ESP32 Hub.

    @details Implements the non-blocking, state-based decoding of frames received on `Serial1`
             (framing `SOF | len_hi | len_lo | payload | crc16_hi | crc16_lo`, see Crc16.h),
             Protobuf payload extraction, idempotent execution/deduplication of
             commands (via `_lastExecutedCmdId`), and sending structured telemetry,
             log and ACK frames to the Hub. It also applies the security policies defined in
             `CommandPolicy.h` (clamp/rate-limit) as a defense independent from the one already
             applied on the Hub side.
*/
class Communication {
public:
    /**
     * @brief Constructor of the Communication class.
     */
    Communication();

    /**
     * @brief Initializes the Serial1 interface at 115200 baud.
     */
    void init();

    /**
     * @brief Reads and processes data arriving on the serial port from the Hub module.
     *
     * To be called periodically in the main loop. Handles frame parsing and triggers
     * execution of the commands extracted from the payload.
     *
     * @param movement Movement module, used to drive the mode or test commands.
     * @param persistence Persistence module, used to save settings and statistics.
     * @param sensors Sensors module, used to read soil moisture, lux, distances.
     * @param pump Pump module, used for irrigation.
     * @param sys Overall system state, used to monitor degraded mode.
     */
    void handleSerial(Movement& movement, Persistence& persistence,
                      Sensors& sensors, Pump& pump, SystemStatus& sys);

    /**
     * @brief Sends a framed Protobuf message (WrapperMessage) over serial.
     * @param message Protobuf wrapper message to send.
     */
    void sendProtobufMessage(const WrapperMessage& message);

    /**
     * @brief Sends the fast telemetry (TelemetryFast) over serial.
     */
    void sendFastTelemetry(const TelemetryFast& tf);

    /**
     * @brief Sends the deep telemetry (TelemetryDeep) over serial.
     */
    void sendDeepTelemetry(const TelemetryDeep& td);

    /**
     * @brief Sends the periodic heartbeat to the Hub.
     * @param uptime_s Uptime in seconds.
     * @param isDegraded Flag indicating whether the system is in degraded mode.
     * @param deviceId Unique device identifier.
     */
    void sendHeartbeat(uint32_t uptime_s, bool isDegraded, const char* deviceId);

    /**
     * @brief Sends the response to a received command (ACK).
     */
    void sendCommandResponse(CommandResponse_Status status, const char* detail,
                             int32_t value, uint32_t cmd_id, uint32_t exec_time_ms);

    /**
     * @brief Queues a log event for sending.
     *
     * Logs are written to a circular queue so as not to block the Mega's CPU execution.
     */
    void logEvent(Log_LogLevel level, const char* event, const char* detail,
                  const char* deviceId, CumulativeStats& stats);

    /**
     * @brief Pops a log entry from the queue and transmits it to the Hub.
     *
     * Called periodically by the non-blocking scheduler.
     */
    void drainLogQueue(const char* deviceId);

    /**
     * @brief Returns the timestamp in ms of the last valid message received from the Hub.
     * @return unsigned long Milliseconds since millis().
     */
    unsigned long getLastHubMessageMs() const { return last_hub_message_ms; }

private:
    /**
     * @brief Wraps a Protobuf payload in the SOF/len/payload/CRC serial frame and sends it.
     */
    void sendFramedMessage(const uint8_t* payload, uint16_t len);

    /**
     * @brief Executes the commands received from the Hub module, applying clamp and deduplication policies.
     */
    void executeCommand(const WrapperMessage& message,
                        Movement& movement, Persistence& persistence,
                        Sensors& sensors, Pump& pump, SystemStatus& sys);

    uint8_t protobuf_tx_buffer[PROTOBUF_BUFFER_SIZE];
    uint8_t protobuf_rx_buffer[PROTOBUF_BUFFER_SIZE];

    unsigned long last_hub_message_ms;  /**< Millis of the last message received */
    unsigned long last_water_ms;         /**< Timestamp of the last irrigation, to prevent accidental over-watering */
    uint32_t      _lastExecutedCmdId;    /**< ID of the last successfully executed command (for idempotency) */
};

/*! @} */ // MegaComm

#endif // COMMUNICATION_H
