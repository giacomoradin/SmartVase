#ifndef SERIALMANAGER_H
#define SERIALMANAGER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// Includi le definizioni Protobuf generate da Nanopb
// Assumendo che siano nella cartella lib/nanopb del progetto PlatformIO
extern "C" {
  #include "pb_decode.h"
  #include "pb_encode.h"
  #include "smartvase.pb.h" // Il tuo file .pb.h generato
}
#include "smartvase_aliases.h" // I tuoi alias se li usi

// Definisci la dimensione massima dei buffer seriali e del payload Protobuf
// Deve essere uguale o maggiore di PROTOBUF_BUFFER_SIZE definito nel firmware Mega
#define SERIAL_BUFFER_SIZE 256
#define PROTOBUF_MAX_PAYLOAD SERIAL_BUFFER_SIZE // Coerenza

// Definisci qui i pin per Serial2 (o altra porta UART che scegli)
#define MEGA_RX_PIN 16 // ESP32 RX <--- Mega TX1 (18)
#define MEGA_TX_PIN 17 // ESP32 TX ---> Mega RX1 (19)

// Struttura per i messaggi scambiati nelle code FreeRTOS
// Semplice wrapper intorno al messaggio Protobuf decodificato/da codificare
typedef struct {
    WrapperMessage message; // Copia del messaggio Protobuf
    // Aggiungi qui altri metadati se necessario (es. timestamp ricezione)
} SerialMessage;


class SerialManager {
public:
    // Costruttore: Inizializza le code
    SerialManager(QueueHandle_t rxQueue, QueueHandle_t txQueue);

    // Funzione statica che fa da entry point per il Task FreeRTOS
    // Deve essere statica perché i task FreeRTOS sono funzioni C-style
    static void taskEntry(void* pvParameters);

    // Inizializza la porta Seriale
    void init();

private:
    // Code FreeRTOS per la gestione dei messaggi in ingresso e uscita
    QueueHandle_t _rxQueue; // Coda per i messaggi RICEVUTI dal Mega
    QueueHandle_t _txQueue; // Coda per i messaggi DA INVIARE al Mega

    // Buffer interni per la ricezione e trasmissione seriale
    uint8_t _rxBuffer[SERIAL_BUFFER_SIZE];
    uint8_t _txBuffer[SERIAL_BUFFER_SIZE];

    // Stato della FSM di ricezione tenuto in MEMBRI (classe rientrante: niente
    // piu' variabili static locali in handleSerialReception()).
    enum RxState { WAIT_SOF, WAIT_LEN_H, WAIT_LEN_L, WAIT_PAYLOAD, WAIT_CRC_H, WAIT_CRC_L };
    RxState  _rxState         = WAIT_SOF;
    uint16_t _rxMessageLength = 0;
    uint16_t _rxPayloadIndex  = 0;
    uint16_t _rxReceivedCRC   = 0;

    // Funzione principale del Task (il loop infinito)
    void taskRun();

    // Gestisce la ricezione dei dati seriali con il protocollo di framing
    void handleSerialReception();

    // Invia un messaggio Protobuf al Mega usando il protocollo di framing
    bool sendProtobufMessage(const WrapperMessage& message);
    // CRC16-CCITT in crc_utils (condiviso con ConfigManager).
};

#endif // SERIALMANAGER_H
