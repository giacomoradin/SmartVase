#include "SerialManager.h"
#include "esp_log.h"
#include <HardwareSerial.h> // Per accedere a Serial2
#include "crc_utils.h"      // crc16_ccitt (condiviso)

// Tag per i log specifici di questo modulo
static const char *TAG = "SerialManager";

// Definizioni per il protocollo di framing (devono corrispondere a quelle del Mega)
#define SOF_BYTE 0xAA // Start Of Frame

// --- Implementazione Metodi Classe SerialManager ---

SerialManager::SerialManager(QueueHandle_t rxQueue, QueueHandle_t txQueue)
    : _rxQueue(rxQueue), _txQueue(txQueue) {
    // Il costruttore riceve le code create in main.cpp
}

void SerialManager::init() {
    // Inizializza Serial2 sui pin definiti
    Serial2.begin(115200, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);
    ESP_LOGI(TAG, "Serial2 initialized (RX:%d, TX:%d) at 115200 baud.", MEGA_RX_PIN, MEGA_TX_PIN);
}

// Funzione statica usata per avviare il task FreeRTOS
void SerialManager::taskEntry(void* pvParameters) {
    // Il parametro è un puntatore all'istanza della classe SerialManager
    SerialManager* instance = static_cast<SerialManager*>(pvParameters);
    // Chiama la funzione membro che contiene il loop del task
    instance->taskRun();
}

// Funzione principale del Task (loop infinito)
void SerialManager::taskRun() {
    ESP_LOGI(TAG, "SerialManager Task Started.");

    // Loop principale del task
    while (true) {
        // 1. Gestisci la ricezione di dati dal Mega
        handleSerialReception();

        // 2. Attendi (max 10 ms) un messaggio da inviare al Mega: se arriva, il
        //    task si sveglia subito (latenza TX ~0); altrimenti il timeout fa da
        //    tick per ripassare a drenare la ricezione seriale.
        SerialMessage msgToSend;
        if (xQueueReceive(_txQueue, &msgToSend, pdMS_TO_TICKS(10)) == pdPASS) {
            if (!sendProtobufMessage(msgToSend.message)) {
                ESP_LOGE(TAG, "Failed to send protobuf message!");
            }
        }
    }
}

// Gestisce la ricezione dei dati seriali con il protocollo di framing (SOF, Len, Payload, CRC)
void SerialManager::handleSerialReception() {
    // Stato FSM in MEMBRI (classe rientrante). Alias locali per non riscrivere
    // tutto il corpo della funzione.
    RxState&  state         = _rxState;
    uint16_t& messageLength = _rxMessageLength;
    uint16_t& payloadIndex  = _rxPayloadIndex;
    uint16_t& receivedCRC   = _rxReceivedCRC;

    // Leggi tutti i byte disponibili dalla porta seriale
    while (Serial2.available() > 0) {
        uint8_t byteIn = Serial2.read();

        switch (state) {
            case WAIT_SOF:
                if (byteIn == SOF_BYTE) {
                    state = WAIT_LEN_H;
                    payloadIndex = 0; // Resetta l'indice per un nuovo messaggio
                    messageLength = 0;
                    receivedCRC = 0;
                     ESP_LOGV(TAG, "SOF received");
                } else {
                     ESP_LOGV(TAG, "Waiting SOF, received 0x%02X", byteIn);
                     // Ignora byte spazzatura prima del SOF
                }
                break;

            case WAIT_LEN_H: // Riceve il byte alto della lunghezza
                messageLength = (uint16_t)byteIn << 8;
                state = WAIT_LEN_L;
                break;

            case WAIT_LEN_L: // Riceve il byte basso della lunghezza
                messageLength |= byteIn;
                ESP_LOGV(TAG, "Length received: %d", messageLength);
                // Controllo di sanità sulla lunghezza
                if (messageLength == 0 || messageLength > sizeof(_rxBuffer)) {
                    ESP_LOGE(TAG, "Invalid message length received: %d. Max allowed: %d", messageLength, sizeof(_rxBuffer));
                    state = WAIT_SOF; // Torna in attesa del prossimo SOF
                } else {
                    payloadIndex = 0; // Prepara a ricevere il payload
                    state = WAIT_PAYLOAD;
                }
                break;

            case WAIT_PAYLOAD: // Riceve i byte del payload Protobuf
                _rxBuffer[payloadIndex++] = byteIn;
                // Se abbiamo ricevuto tutti i byte del payload
                if (payloadIndex == messageLength) {
                    state = WAIT_CRC_H; // Passa a ricevere il CRC
                     ESP_LOGV(TAG, "Payload received (%d bytes)", messageLength);
                }
                break;

            case WAIT_CRC_H: // Riceve il byte alto del CRC
                receivedCRC = (uint16_t)byteIn << 8;
                state = WAIT_CRC_L;
                break;

            case WAIT_CRC_L: // Riceve il byte basso del CRC e valida il messaggio
            { // <-- APERTURA PARENTESI GRAFFA
                receivedCRC |= byteIn;
                ESP_LOGV(TAG, "CRC received: 0x%04X", receivedCRC);

                // Calcola il CRC sul payload ricevuto
                uint16_t calculatedCRC = crc16_ccitt(_rxBuffer, messageLength);

                if (receivedCRC == calculatedCRC) {
                    // CRC Corretto! Decodifica il messaggio Protobuf
                    ESP_LOGD(TAG, "CRC OK. Decoding Protobuf message...");
                    WrapperMessage decodedMsg = WrapperMessage_init_zero;
                    pb_istream_t stream = pb_istream_from_buffer(_rxBuffer, messageLength);

                    if (pb_decode(&stream, WrapperMessage_fields, &decodedMsg)) {
                        // Decodifica riuscita! Inserisci il messaggio nella coda rxQueue
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
                break; // Il break rimane DENTRO le graffe
            } // <-- CHIUSURA PARENTESI GRAFFA spostata QUI!

            // --- CORREZIONE: La chiusura della graffa era qui per errore ---

            default: // Ora 'default' è correttamente all'interno dello switch
                ESP_LOGE(TAG, "Invalid state in serial reception FSM: %d! Resetting to WAIT_SOF.", state);
                state = WAIT_SOF;
                break;
        } // end switch
    } // end while (Serial2.available())
}


// Invia un messaggio Protobuf al Mega usando il protocollo di framing
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

// CRC16-CCITT spostato in crc_utils.{h,cpp} (condiviso con ConfigManager).
// Resta poly 0x1021 MSB-first, identico a crc16_ccitt del Mega: il test host
// `test_crc_utils` pinna il valore (XMODEM 0x31C3) contro regressioni.

