/*! @file SerialManager.cpp
 *  @ingroup HubSerial
 *  @brief Implementation of SerialManager: serial framing receive FSM and
 *  sending of Protobuf messages to the Mega.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

#include "SerialManager.h"
#include "esp_log.h"
#include <HardwareSerial.h> // To access Serial2
#include "crc_utils.h"      // crc16_ccitt (shared)

// Tag for this module's log messages
static const char *TAG = "SerialManager";

/*! @brief Start-of-frame of the serial protocol (must match the Mega's). */
#define SOF_BYTE 0xAA

// --- Implementazione Metodi Classe SerialManager ---

SerialManager::SerialManager(QueueHandle_t rxQueue, QueueHandle_t txQueue)
    : _rxQueue(rxQueue), _txQueue(txQueue) {
    // The constructor receives the queues created in main.cpp
}

void SerialManager::init() {
    // Initialize Serial2 on the defined pins
    Serial2.begin(115200, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);
    ESP_LOGI(TAG, "Serial2 initialized (RX:%d, TX:%d) at 115200 baud.", MEGA_RX_PIN, MEGA_TX_PIN);
}

// Static function used to start the FreeRTOS task
void SerialManager::taskEntry(void* pvParameters) {
    // The parameter is a pointer to the SerialManager instance
    SerialManager* instance = static_cast<SerialManager*>(pvParameters);
    // Call the member function containing the task loop
    instance->taskRun();
}

// Main task function (infinite loop)
void SerialManager::taskRun() {
    ESP_LOGI(TAG, "SerialManager Task Started.");

    // Main task loop
    while (true) {
        // 1. Handle incoming data from the Mega
        handleSerialReception();

        // 2. Wait (max 10 ms) for a message to send to the Mega: if one arrives,
        //    the task wakes up immediately (~0 TX latency); otherwise the
        //    timeout acts as a tick to go back and drain the serial reception.
        SerialMessage msgToSend;
        if (xQueueReceive(_txQueue, &msgToSend, pdMS_TO_TICKS(10)) == pdPASS) {
            if (!sendProtobufMessage(msgToSend.message)) {
                ESP_LOGE(TAG, "Failed to send protobuf message!");
            }
        }
    }
}

// Handles reception of serial data using the framing protocol (SOF, Len, Payload, CRC)
void SerialManager::handleSerialReception() {
    // FSM state kept as instance MEMBERS (reentrant class). Local aliases so
    // we don't have to rewrite the whole function body.
    RxState&  state         = _rxState;
    uint16_t& messageLength = _rxMessageLength;
    uint16_t& payloadIndex  = _rxPayloadIndex;
    uint16_t& receivedCRC   = _rxReceivedCRC;

    // Read all bytes currently available on the serial port
    while (Serial2.available() > 0) {
        uint8_t byteIn = Serial2.read();

        switch (state) {
            case WAIT_SOF:
                if (byteIn == SOF_BYTE) {
                    state = WAIT_LEN_H;
                    payloadIndex = 0; // Reset the index for a new message
                    messageLength = 0;
                    receivedCRC = 0;
                     ESP_LOGV(TAG, "SOF received");
                } else {
                     ESP_LOGV(TAG, "Waiting SOF, received 0x%02X", byteIn);
                     // Ignore junk bytes before the SOF
                }
                break;

            case WAIT_LEN_H: // Receives the high byte of the length
                messageLength = (uint16_t)byteIn << 8;
                state = WAIT_LEN_L;
                break;

            case WAIT_LEN_L: // Receives the low byte of the length
                messageLength |= byteIn;
                ESP_LOGV(TAG, "Length received: %d", messageLength);
                // Sanity check on the length
                if (messageLength == 0 || messageLength > sizeof(_rxBuffer)) {
                    ESP_LOGE(TAG, "Invalid message length received: %d. Max allowed: %d", messageLength, sizeof(_rxBuffer));
                    state = WAIT_SOF; // Go back to waiting for the next SOF
                } else {
                    payloadIndex = 0; // Prepare to receive the payload
                    state = WAIT_PAYLOAD;
                }
                break;

            case WAIT_PAYLOAD: // Receives the Protobuf payload bytes
                _rxBuffer[payloadIndex++] = byteIn;
                // If we've received all the payload bytes
                if (payloadIndex == messageLength) {
                    state = WAIT_CRC_H; // Move on to receiving the CRC
                     ESP_LOGV(TAG, "Payload received (%d bytes)", messageLength);
                }
                break;

            case WAIT_CRC_H: // Receives the high byte of the CRC
                receivedCRC = (uint16_t)byteIn << 8;
                state = WAIT_CRC_L;
                break;

            case WAIT_CRC_L: // Receives the low byte of the CRC and validates the message
            { // <-- OPENING BRACE
                receivedCRC |= byteIn;
                ESP_LOGV(TAG, "CRC received: 0x%04X", receivedCRC);

                // Compute the CRC over the received payload
                uint16_t calculatedCRC = crc16_ccitt(_rxBuffer, messageLength);

                if (receivedCRC == calculatedCRC) {
                    // CRC OK! Decode the Protobuf message
                    ESP_LOGD(TAG, "CRC OK. Decoding Protobuf message...");
                    WrapperMessage decodedMsg = WrapperMessage_init_zero;
                    pb_istream_t stream = pb_istream_from_buffer(_rxBuffer, messageLength);

                    if (pb_decode(&stream, WrapperMessage_fields, &decodedMsg)) {
                        // Decode succeeded! Push the message onto the rxQueue
                        SerialMessage serialMsg;
                        memcpy(&serialMsg.message, &decodedMsg, sizeof(WrapperMessage));

                        if (xQueueSend(_rxQueue, &serialMsg, (TickType_t)10) != pdPASS) {
                            ESP_LOGW(TAG, "RX Queue is full! Discarding incoming message.");
                        } else {
                             ESP_LOGD(TAG, "Message placed in RX Queue.");
                        }
                    } else {
                        ESP_LOGE(TAG, "Protobuf decoding failed: %s", PB_GET_ERROR(&stream));
                    }
                } else {
                    ESP_LOGE(TAG, "CRC Error! Expected 0x%04X, Calculated 0x%04X", receivedCRC, calculatedCRC);
                }
                state = WAIT_SOF;
                break;
            }

            default:
                ESP_LOGE(TAG, "Invalid state in serial reception FSM: %d! Resetting to WAIT_SOF.", state);
                state = WAIT_SOF;
                break;
        } // end switch
    } // end while (Serial2.available())
}


// Sends a Protobuf message to the Mega using the framing protocol
bool SerialManager::sendProtobufMessage(const WrapperMessage& message) {
    pb_ostream_t stream = pb_ostream_from_buffer(_txBuffer, sizeof(_txBuffer));

    if (!pb_encode(&stream, WrapperMessage_fields, &message)) {
        const char* error_msg = PB_GET_ERROR(&stream);
        ESP_LOGE(TAG, "Protobuf encoding failed! Error: %s. Buffer size %d maybe too small?",
                 (error_msg ? error_msg : "Unknown error"), sizeof(_txBuffer));
        return false;
    }

    uint16_t payloadLen = stream.bytes_written;
    if (payloadLen == 0) {
        ESP_LOGW(TAG, "Attempted to send an empty protobuf message (encoding resulted in 0 bytes).");
        return true;
    }
     if (payloadLen > sizeof(_txBuffer)) {
         ESP_LOGE(TAG, "Protobuf encoding overflowed buffer! Encoded size: %d, Buffer size: %d", payloadLen, sizeof(_txBuffer));
         return false;
     }

    ESP_LOGD(TAG, "Encoding successful (%d bytes). Sending framed message...", payloadLen);
    uint16_t crc = crc16_ccitt(_txBuffer, payloadLen);

    Serial2.write(SOF_BYTE);
    Serial2.write((uint8_t)(payloadLen >> 8));
    Serial2.write((uint8_t)(payloadLen & 0xFF));
    Serial2.write(_txBuffer, payloadLen);
    Serial2.write((uint8_t)(crc >> 8));
    Serial2.write((uint8_t)(crc & 0xFF));
    Serial2.flush();

    ESP_LOGD(TAG, "Framed message sent (SOF + Len + %d bytes Payload + CRC=0x%04X).", payloadLen, crc);
    return true;
}

// CRC16-CCITT moved to crc_utils.{h,cpp} (shared with ConfigManager).
// Still poly 0x1021 MSB-first, identical to the Mega's crc16_ccitt: the host
// test `test_crc_utils` pins the value (XMODEM 0x31C3) against regressions.

