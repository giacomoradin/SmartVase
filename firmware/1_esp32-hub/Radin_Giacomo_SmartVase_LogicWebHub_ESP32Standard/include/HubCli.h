#ifndef HUBCLI_H
#define HUBCLI_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class ConfigManager;
class WifiManager;
class MqttManager;
class MainLogic;

// CLI seriale di debug e provisioning sulla porta USB (115200, '\n').
// Gira nel task loop() di Arduino (priorita' minima): legge senza bloccare.
//
//   help / ?                  lista comandi
//   version                   versione firmware
//   status                    Wi-Fi, MQTT, link Mega, heap, uptime
//   show                      configurazione NVS (password mascherate)
//   set <chiave> <valore>     wifi_ssid|wifi_pass|mqtt_broker|mqtt_port|
//                             mqtt_user|mqtt_pass
//   save                      persiste la configurazione su NVS
//   wifi connect              ritenta la connessione STA
//   reboot                    riavvia l'ESP32
//
// Comandi passthrough verso il Mega (testano l'intera catena seriale
// Protobuf anche senza rete; l'esito arriva come [ACK Mega] sul monitor):
//   telemetry                 ultima TelemetryFast/Deep ricevuta dal Mega
//   water <ms>                avvia irrigazione (protezione tanica sul Mega)
//   mode <idle|light|shadow>  cambia modalita' movimento
//   stop                      ferma motori e pompa
//   soil                      legge umidita' suolo
//   diag                      richiede TelemetryDeep immediata
//   megareset                 soft reset del Mega
class HubCli {
public:
    HubCli();
    void begin(ConfigManager* cfg, WifiManager* wifi, MqttManager* mqtt,
               MainLogic* logic, QueueHandle_t serialTxQueue);
    void tick();

private:
    void execute(char* line);
    void printHelp();
    void printStatus();
    void printShow();
    void printTelemetry();
    bool handleSet(char* args);
    // Invia un Command protobuf al Mega via coda TX seriale.
    void sendMegaCommand(uint8_t which, uint32_t arg);

    ConfigManager* _cfg;
    WifiManager*   _wifi;
    MqttManager*   _mqtt;
    MainLogic*     _logic;
    QueueHandle_t  _serialTxQueue;
    uint32_t       _nextCmdId;

    static const size_t BUF_SIZE = 192;
    char   buf[BUF_SIZE];
    size_t pos;
};

#endif // HUBCLI_H
