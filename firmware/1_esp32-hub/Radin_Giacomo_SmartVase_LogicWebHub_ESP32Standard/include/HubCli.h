/*! @file HubCli.h
 *  @ingroup HubCli
 *  @brief CLI seriale di debug/provisioning/diagnostica per l'Hub, esposta su
 *  USB (115200 baud).
 *  @author Giacomo Radin
 *  @date 2026-06-11
 */

/*! @defgroup HubCli CLI seriale di debug
 *  @brief Interfaccia testuale su USB per ispezionare lo stato dell'Hub,
 *  fare il provisioning di Wi-Fi/MQTT e inviare comandi di test al Mega anche
 *  in laboratorio senza rete.
 */

#ifndef HUBCLI_H
#define HUBCLI_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class ConfigManager;
class WifiManager;
class MqttManager;
class MainLogic;

/*! @addtogroup HubCli
 *  @{
 */

/*!
 * @class HubCli
 * @brief CLI seriale di debug e provisioning sulla porta USB (115200 baud,
 * terminatore '\\n').
 * @details Gira interamente nel loop() Arduino (priorita' minima): tick()
 * legge i caratteri disponibili senza bloccare e accumula una riga in `buf`
 * fino al terminatore. Comandi gestiti localmente:
 *
 *   help / ?                  lista comandi
 *   version                   versione firmware
 *   status                    Wi-Fi, MQTT, link Mega, heap, uptime
 *   show                      configurazione NVS (password mascherate)
 *   set <chiave> <valore>     wifi_ssid|wifi_pass|mqtt_broker|mqtt_port|
 *                             mqtt_user|mqtt_pass
 *   save                      persiste la configurazione su NVS
 *   wifi connect              ritenta la connessione STA
 *   reboot                    riavvia l'ESP32
 *
 * Comandi passthrough verso il Mega (testano l'intera catena seriale
 * Protobuf anche senza rete; l'esito arriva come [ACK Mega] sul monitor,
 * stampato da MainLogic::processSerialMessage()):
 *
 *   telemetry                 ultima TelemetryFast/Deep ricevuta dal Mega
 *   water <ms>                avvia irrigazione (protezione tanica sul Mega)
 *   mode <idle|light|shadow>  cambia modalita' movimento
 *   stop                      ferma motori e pompa
 *   soil                      legge umidita' suolo
 *   megadiag                  richiede una TelemetryDeep immediata
 *   megareset                 soft reset del Mega
 *
 * @note Tutti i puntatori (`_cfg`, `_wifi`, `_mqtt`, `_logic`) sono passati per
 * riferimento da `begin()` e devono restare validi per tutta la vita
 * dell'oggetto: in main.cpp sono istanze/puntatori globali con lifetime pari
 * al programma.
 */
class HubCli {
public:
    /*! @brief Costruttore: azzera i puntatori ai moduli e il buffer di riga
     *  (oggetto non operativo finche' begin() non viene chiamato). */
    HubCli();

    /*! @brief Collega la CLI ai moduli dell'Hub e abilita tick().
     *  @param[in] cfg Gestore configurazione NVS, per i comandi `show`/`set`/`save`.
     *  @param[in] wifi Gestore Wi-Fi, per `wifi connect`.
     *  @param[in] mqtt Gestore MQTT, per `status`/`diag`.
     *  @param[in] logic Core logico, per leggere telemetria/stato link Mega e
     *  il device ID.
     *  @param[in] serialTxQueue Coda verso il Mega, usata da sendMegaCommand()
     *  per i comandi passthrough.
     *  @note Da chiamare una sola volta in setup(), dopo l'inizializzazione di
     *  tutti i moduli passati. */
    void begin(ConfigManager* cfg, WifiManager* wifi, MqttManager* mqtt,
               MainLogic* logic, QueueHandle_t serialTxQueue);

    /*! @brief Da chiamare ad ogni giro di loop(): legge i caratteri disponibili
     *  su `Serial` senza bloccare, accumula una riga e la esegue al terminatore '\\n'.
     *  @note No-op se begin() non e' ancora stato chiamato (`_cfg == nullptr`).
     *  Una riga piu' lunga di BUF_SIZE-1 viene scartata con un messaggio di errore. */
    void tick();

private:
    /*! @brief Effettua il dispatch di una riga di comando completa (NUL-terminated, senza '\\n').
     *  @param[in] line Buffer mutabile (alcuni comandi tokenizzano in-place con strtok/strchr). */
    void execute(char* line);
    /*! @brief Stampa l'elenco dei comandi disponibili (comando `help`/`?`). */
    void printHelp();
    /*! @brief Stampa un riepilogo rapido di stato: Wi-Fi, MQTT, link Mega, heap, uptime (comando `status`). */
    void printStatus();
    /*! @brief Stampa una diagnostica guidata (con suggerimenti) di Wi-Fi/NTP/MQTT/link Mega (comando `diag`). */
    void printDiag();
    /*! @brief Stampa la configurazione NVS corrente, con le password mascherate (comando `show`). */
    void printShow();
    /*! @brief Stampa l'ultima TelemetryFast/Deep ricevuta dal Mega, o un avviso se non ancora arrivata (comando `telemetry`). */
    void printTelemetry();
    /*! @brief Implementa il comando `set <chiave> <valore>`: aggiorna in memoria
     *  il campo di configurazione richiesto (richiede poi `save` per persisterlo).
     *  @param[in,out] args Stringa "<chiave> <valore>"; viene modificata in-place
     *  (il primo spazio e' sostituito con '\\0' per separare chiave e valore).
     *  @return true se la chiave e' riconosciuta e il valore sintatticamente valido,
     *  false altrimenti (il chiamante stampa l'usage). */
    bool handleSet(char* args);
    /*! @brief Costruisce un `Command` Protobuf e lo accoda su `_serialTxQueue` per il Mega.
     *  @param[in] which Tag del campo oneof `command_type` (es. Command_water_tag).
     *  @param[in] arg Argomento del comando (interpretato in base a `which`: durata
     *  in ms per water, valore enum per set_mode, ignorato per i comandi senza payload).
     *  @note Usa un cmd_id auto-incrementante a partire da 9000 (`_nextCmdId`) per
     *  distinguere le risposte dei comandi originati da CLI da quelli MQTT. */
    void sendMegaCommand(uint8_t which, uint32_t arg);

    ConfigManager* _cfg;   /**< Gestore configurazione NVS (non posseduto). */
    WifiManager*   _wifi;  /**< Gestore Wi-Fi (non posseduto). */
    MqttManager*   _mqtt;  /**< Gestore MQTT (non posseduto). */
    MainLogic*     _logic; /**< Core logico (non posseduto). */
    QueueHandle_t  _serialTxQueue; /**< Coda verso il Mega per i comandi passthrough. */
    uint32_t       _nextCmdId;     /**< Prossimo cmd_id da assegnare ai comandi inviati da CLI (parte da 9000). */

    static const size_t BUF_SIZE = 192; /**< Capacita' massima di una riga di comando, incluso il terminatore. */
    char   buf[BUF_SIZE]; /**< Buffer di accumulo della riga corrente. */
    size_t pos;            /**< Numero di caratteri attualmente accumulati in buf. */
};

/*! @} */ // end of HubCli group

#endif // HUBCLI_H
