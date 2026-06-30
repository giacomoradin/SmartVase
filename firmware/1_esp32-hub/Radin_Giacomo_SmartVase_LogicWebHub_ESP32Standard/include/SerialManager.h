/*! @file SerialManager.h
 *  @ingroup HubSerial
 *  @brief Framing seriale Hub<->Mega: incapsula/decapsula messaggi Protobuf
 *  `WrapperMessage` su UART2 con header SOF/lunghezza e trailer CRC16-CCITT.
 *  @author Giacomo Radin
 *  @date 2025-10-28
 */

/*! @defgroup HubSerial Comunicazione seriale con il Mega
 *  @brief Protocollo di framing (SOF, lunghezza, payload Protobuf, CRC16-CCITT)
 *  usato sul collegamento UART2 tra l'Hub e l'Arduino Mega, e le utility CRC
 *  condivise.
 */

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

/*! @addtogroup HubSerial
 *  @{
 */

/*!
 * @def SERIAL_BUFFER_SIZE
 * @brief Dimensione massima (byte) del payload Protobuf nel framing seriale.
 * @note Deve essere uguale o maggiore di PROTOBUF_BUFFER_SIZE definito nel
 * firmware del Mega, altrimenti un frame valido lato Mega verrebbe scartato
 * qui per superamento buffer (vedi handleSerialReception(), stato WAIT_LEN_L).
 */
#define SERIAL_BUFFER_SIZE 256
#define PROTOBUF_MAX_PAYLOAD SERIAL_BUFFER_SIZE // Coerenza

/*! @name Pin UART2 verso il Mega
 *  @{
 */
#define MEGA_RX_PIN 16 // ESP32 RX <--- Mega TX1 (18)
#define MEGA_TX_PIN 17 // ESP32 TX ---> Mega RX1 (19)
/*! @} */

/*!
 * @struct SerialMessage
 * @brief Wrapper attorno a un `WrapperMessage` Protobuf, usato come tipo
 * elemento delle code FreeRTOS `serialRxQueue`/`serialTxQueue` (vedi main.cpp).
 */
typedef struct {
    WrapperMessage message; /**< Copia del messaggio Protobuf decodificato (RX) o da codificare (TX). */
} SerialMessage;

/*!
 * @class SerialManager
 * @brief Corpo del task FreeRTOS `TaskSerialMega`: gestisce UART2 verso il
 * Mega, decodifica i frame in ingresso e incapsula/spedisce quelli in uscita.
 * @details Framing: `0xAA | len_hi | len_lo | payload Protobuf (<=SERIAL_BUFFER_SIZE) | crc16_hi | crc16_lo`,
 * con CRC16-CCITT (poly 0x1021, init 0x0000, MSB-first) calcolato sul solo
 * payload. Un frame con CRC o lunghezza non validi viene scartato silenziosamente
 * (con log a livello ERROR) e la FSM di ricezione torna in attesa del prossimo SOF.
 * @note Una sola istanza gira nel task TaskSerialMega (Core 1, priorita' alta):
 * lo stato della FSM RX e i buffer sono membri di istanza (non static locali),
 * per rendere la classe rientrante in linea di principio, ma non e' pensata
 * per essere usata da piu' di un task contemporaneamente.
 */
class SerialManager {
public:
    /*! @brief Costruttore: salva gli handle delle code FreeRTOS create in main.cpp.
     *  @param[in] rxQueue Coda su cui pubblicare i messaggi Protobuf ricevuti dal Mega.
     *  @param[in] txQueue Coda da cui leggere i messaggi Protobuf da inviare al Mega. */
    SerialManager(QueueHandle_t rxQueue, QueueHandle_t txQueue);

    /*! @brief Entry point statico per `xTaskCreatePinnedToCore`: inoltra alla taskRun() dell'istanza.
     *  @param[in] pvParameters Puntatore alla SerialManager su cui girare il task. */
    static void taskEntry(void* pvParameters);

    /*! @brief Inizializza Serial2 (UART2) a 115200 baud sui pin MEGA_RX_PIN/MEGA_TX_PIN.
     *  @note Da chiamare una sola volta in setup(), prima della creazione del task TaskSerialMega. */
    void init();

private:
    QueueHandle_t _rxQueue; /**< Coda per i messaggi RICEVUTI dal Mega (consumata da MainLogic). */
    QueueHandle_t _txQueue; /**< Coda per i messaggi DA INVIARE al Mega (alimentata da MainLogic/HubCli). */

    uint8_t _rxBuffer[SERIAL_BUFFER_SIZE]; /**< Buffer di accumulo del payload Protobuf in ricezione. */
    uint8_t _txBuffer[SERIAL_BUFFER_SIZE]; /**< Buffer di codifica del payload Protobuf in trasmissione. */

    /*! @brief Stati della FSM di ricezione del framing seriale. */
    enum RxState {
        WAIT_SOF,     /**< In attesa del byte di Start Of Frame (0xAA). */
        WAIT_LEN_H,   /**< Atteso il byte alto della lunghezza payload. */
        WAIT_LEN_L,   /**< Atteso il byte basso della lunghezza payload; qui si valida il range. */
        WAIT_PAYLOAD, /**< Accumulo dei byte del payload Protobuf in _rxBuffer. */
        WAIT_CRC_H,   /**< Atteso il byte alto del CRC16. */
        WAIT_CRC_L    /**< Atteso il byte basso del CRC16; qui si valida e si decodifica il frame. */
    };
    // Stato della FSM di ricezione tenuto in MEMBRI (classe rientrante: niente
    // piu' variabili static locali in handleSerialReception()).
    RxState  _rxState         = WAIT_SOF; /**< Stato corrente della FSM RX. */
    uint16_t _rxMessageLength = 0;        /**< Lunghezza payload dichiarata dal frame corrente. */
    uint16_t _rxPayloadIndex  = 0;        /**< Numero di byte payload gia' accumulati in _rxBuffer. */
    uint16_t _rxReceivedCRC   = 0;        /**< CRC16 letto dal frame, da confrontare col CRC calcolato. */

    /*! @brief Corpo del task FreeRTOS `TaskSerialMega`: alterna lettura non
     *  bloccante della seriale (handleSerialReception()) e invio (con timeout
     *  10 ms) di eventuali messaggi presenti in `_txQueue`. */
    void taskRun();

    /*! @brief Avanza la FSM di ricezione consumando tutti i byte disponibili su Serial2.
     *  @details Alla ricezione di un frame completo con CRC valido, decodifica
     *  il payload come `WrapperMessage` (nanopb) e lo inserisce in `_rxQueue`.
     *  Frame con lunghezza fuori range, CRC errato o decodifica Protobuf fallita
     *  vengono scartati (log ERROR) senza propagare l'errore al chiamante. */
    void handleSerialReception();

    /*! @brief Codifica un `WrapperMessage` in Protobuf e lo spedisce sul Mega
     *  con il framing SOF/lunghezza/payload/CRC16-CCITT.
     *  @param[in] message Messaggio da inviare.
     *  @return true se la codifica e l'invio sono andati a buon fine (incluso il
     *  caso limite di messaggio vuoto, loggato ma non considerato un errore);
     *  false se la codifica fallisce o eccede SERIAL_BUFFER_SIZE.
     *  @note Chiama `Serial2.flush()`: bloccante fino al completamento della
     *  trasmissione fisica dei byte sul buffer TX hardware. */
    bool sendProtobufMessage(const WrapperMessage& message);
    // CRC16-CCITT in crc_utils (condiviso con ConfigManager).
};

/*! @} */ // end of HubSerial group

#endif // SERIALMANAGER_H
