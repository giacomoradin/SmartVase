/*! @file HubCli.cpp
 *  @ingroup HubCli
 *  @brief Implementation of HubCli: command parsing, local commands and
 *  passthrough to the Mega.
 *  @author Giacomo Radin
 *  @date 2026-06-11
 */

#include "HubCli.h"

#include <WiFi.h>
#include <time.h>
#include "ConfigManager.h"
#include "WifiManager.h"
#include "MQTTManager.h"
#include "MainLogic.h"
#include "SerialManager.h"

/*! @brief Versione firmware Hub mostrata dalla CLI (allineata a MainLogic.cpp). */
#define HUB_FW_VERSION "1.3.0"

HubCli::HubCli()
    : _cfg(nullptr), _wifi(nullptr), _mqtt(nullptr), _logic(nullptr),
      _serialTxQueue(nullptr), _nextCmdId(9000), pos(0) {
    buf[0] = '\0';
}

void HubCli::begin(ConfigManager* cfg, WifiManager* wifi, MqttManager* mqtt,
                   MainLogic* logic, QueueHandle_t serialTxQueue) {
    _cfg           = cfg;
    _wifi          = wifi;
    _mqtt          = mqtt;
    _logic         = logic;
    _serialTxQueue = serialTxQueue;
    Serial.println(F("[CLI] pronta: digita 'help'"));
    Serial.print(F("> "));
}

void HubCli::tick() {
    if (_cfg == nullptr) return; // begin() non ancora chiamato
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) execute(buf);
            pos = 0;
            buf[0] = '\0';
            Serial.print(F("> "));
        } else if (c == 8 || c == 127) { // Backspace handling
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                Serial.print(F("\b \b")); // Erase character on terminal screen
            }
        } else if (c >= 32 && c <= 126) { // Printable ASCII only
            if (pos < BUF_SIZE - 1) {
                buf[pos++] = c;
            } else {
                pos = 0;
                buf[0] = '\0';
                Serial.println(F("\n[CLI] riga troppo lunga, scartata"));
                Serial.print(F("> "));
            }
        }
    }
}

void HubCli::execute(char* line) {
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { printHelp();   return; }
    if (strcmp(line, "version") == 0) {
        Serial.println(F("SmartVase Hub v" HUB_FW_VERSION " (" __DATE__ " " __TIME__ ")"));
        return;
    }
    if (strcmp(line, "status")    == 0) { printStatus();    return; }
    if (strcmp(line, "diag")      == 0) { printDiag();      return; }
    if (strcmp(line, "show")      == 0) { printShow();      return; }
    if (strcmp(line, "telemetry") == 0) { printTelemetry(); return; }
    if (strcmp(line, "save") == 0) {
        Serial.println(_cfg->saveConfig() ? F("[CLI] config salvata su NVS (riavvia con 'reboot')")
                                          : F("[CLI] ERRORE salvataggio NVS"));
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        Serial.println(F("[CLI] riavvio..."));
        delay(200);
        ESP.restart();
        return;
    }
    if (strcmp(line, "wifi connect") == 0) {
        _wifi->connect();
        return;
    }
    if (strncmp(line, "set ", 4) == 0) {
        if (!handleSet(line + 4)) {
            Serial.println(F("[CLI] usage: set <wifi_ssid|wifi_pass|mqtt_broker|mqtt_port|mqtt_user|mqtt_pass> <valore>"));
        }
        return;
    }

    // --- Passthrough commands to the Mega ---
    if (strncmp(line, "water ", 6) == 0) {
        int ms = atoi(line + 6);
        if (ms <= 0) { Serial.println(F("[CLI] usage: water <ms>")); return; }
        sendMegaCommand(Command_water_tag, (uint32_t)ms);
        return;
    }
    if (strncmp(line, "mode ", 5) == 0) {
        const char* arg = line + 5;
        uint32_t m;
        if      (strcmp(arg, "idle")   == 0) m = SetModeCommand_Mode_IDLE;
        else if (strcmp(arg, "light")  == 0) m = SetModeCommand_Mode_LIGHT;
        else if (strcmp(arg, "shadow") == 0) m = SetModeCommand_Mode_SHADOW;
        else { Serial.println(F("[CLI] usage: mode <idle|light|shadow>")); return; }
        sendMegaCommand(Command_set_mode_tag, m);
        return;
    }
    if (strcmp(line, "stop")      == 0) { sendMegaCommand(Command_stop_tag, 0);                return; }
    if (strcmp(line, "soil")      == 0) { sendMegaCommand(Command_read_soil_tag, 0);           return; }
    if (strcmp(line, "megadiag")  == 0) { sendMegaCommand(Command_request_diagnostics_tag, 0); return; }
    if (strcmp(line, "megareset") == 0) { sendMegaCommand(Command_soft_reset_tag, 0);          return; }

    Serial.print(F("[CLI] comando sconosciuto: '"));
    Serial.print(line);
    Serial.println(F("' (prova 'help')"));
}

bool HubCli::handleSet(char* args) {
    char* space = strchr(args, ' ');
    if (space == nullptr) return false;
    *space = '\0';
    const char* key   = args;
    const char* value = space + 1;
    if (strlen(value) == 0) return false;

    // ConfigManager's setters work on groups: the current values are read
    // back and the group is rewritten with only the changed field.
    if (strcmp(key, "wifi_ssid") == 0) {
        _cfg->setWifiCredentials(value, _cfg->getWifiPassword());
    } else if (strcmp(key, "wifi_pass") == 0) {
        _cfg->setWifiCredentials(_cfg->getWifiSsid(), value);
    } else if (strcmp(key, "mqtt_broker") == 0) {
        _cfg->setMqttConfig(value, _cfg->getMqttPort(),
                            _cfg->getMqttUser(), _cfg->getMqttPassword());
    } else if (strcmp(key, "mqtt_port") == 0) {
        int p = atoi(value);
        if (p <= 0 || p > 65535) { Serial.println(F("[CLI] porta non valida")); return true; }
        _cfg->setMqttConfig(_cfg->getMqttBroker(), (uint16_t)p,
                            _cfg->getMqttUser(), _cfg->getMqttPassword());
    } else if (strcmp(key, "mqtt_user") == 0) {
        _cfg->setMqttConfig(_cfg->getMqttBroker(), _cfg->getMqttPort(),
                            value, _cfg->getMqttPassword());
    } else if (strcmp(key, "mqtt_pass") == 0) {
        _cfg->setMqttConfig(_cfg->getMqttBroker(), _cfg->getMqttPort(),
                            _cfg->getMqttUser(), value);
    } else {
        return false;
    }
    Serial.print(F("[CLI] "));
    Serial.print(key);
    Serial.println(F(" impostato (ricorda 'save' + 'reboot')"));
    return true;
}

void HubCli::sendMegaCommand(uint8_t which, uint32_t arg) {
    SerialMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.message.which_payload = WrapperMessage_command_tag;
    Command& cmd = msg.message.payload.command;
    cmd.cmd_id = _nextCmdId++;
    cmd.which_command_type = which;
    switch (which) {
        case Command_water_tag:
            cmd.command_type.water.duration_ms = arg;
            break;
        case Command_set_mode_tag:
            cmd.command_type.set_mode.mode = (SetModeCommand_Mode)arg;
            break;
        default:
            break; // comandi senza payload
    }
    if (xQueueSend(_serialTxQueue, &msg, pdMS_TO_TICKS(100)) == pdPASS) {
        Serial.print(F("[CLI] comando inviato al Mega (cmd_id="));
        Serial.print(cmd.cmd_id);
        Serial.println(F("), attendi [ACK Mega]"));
    } else {
        Serial.println(F("[CLI] ERRORE: coda TX verso il Mega piena"));
    }
}

/*! @brief Prints a label to Serial followed by a masked value (only the
 *  first and last character visible, the rest replaced by "***"), so that
 *  passwords/secrets are never fully exposed on the serial monitor.
 *  @param[in] label Text printed before the value (e.g. "wifi_pass   = ").
 *  @param[in] value String to mask; if empty, prints "(vuoto)". */
static void printMasked(const char* label, const char* value) {
    Serial.print(label);
    size_t n = strlen(value);
    if (n == 0) {
        Serial.println(F("(vuoto)"));
    } else {
        Serial.print(value[0]);
        Serial.print(F("***"));
        Serial.println(value[n - 1]);
    }
}

void HubCli::printShow() {
    Serial.println(F("--- config NVS ---"));
    Serial.print(F("wifi_ssid   = ")); Serial.println(_cfg->getWifiSsid());
    printMasked("wifi_pass   = ", _cfg->getWifiPassword());
    Serial.print(F("mqtt_broker = ")); Serial.println(_cfg->getMqttBroker());
    Serial.print(F("mqtt_port   = ")); Serial.println(_cfg->getMqttPort());
    Serial.print(F("mqtt_user   = ")); Serial.println(_cfg->getMqttUser());
    printMasked("mqtt_pass   = ", _cfg->getMqttPassword());
}

void HubCli::printStatus() {
    Serial.println(F("--- status ---"));
    Serial.print(F("fw_version  = ")); Serial.println(F(HUB_FW_VERSION));
    Serial.print(F("device_id   = ")); Serial.println(_logic->deviceId());
    Serial.print(F("wifi        = "));
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("CONNESSO  ip=")); Serial.print(WiFi.localIP());
        Serial.print(F(" rssi="));        Serial.println(WiFi.RSSI());
    } else {
        Serial.println(F("OFFLINE"));
    }
    Serial.print(F("mqtt        = "));
    if (!_mqtt->isConfigured())     Serial.println(F("NON CONFIGURATO"));
    else if (_mqtt->isConnected())  Serial.println(F("CONNESSO"));
    else                            Serial.println(F("DISCONNESSO"));
    Serial.print(F("mega_link   = "));
    if (_logic->isMegaConnected()) {
        Serial.print(F("OK (ultimo msg "));
        Serial.print(_logic->megaLastMessageAgeMs() / 1000UL);
        Serial.println(F(" s fa)"));
    } else {
        Serial.println(F("ASSENTE (nessun messaggio dal Mega)"));
    }
    Serial.print(F("free_heap   = ")); Serial.print(ESP.getFreeHeap()); Serial.println(F(" B"));
    Serial.print(F("uptime_s    = ")); Serial.println(millis() / 1000UL);
}

void HubCli::printDiag() {
    Serial.println(F("============== DIAGNOSTICA Hub =============="));

    // --- Wi-Fi ---
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    Serial.print(F("[WiFi] "));
    if (wifiUp) {
        Serial.print(F("CONNESSO ip=")); Serial.print(WiFi.localIP());
        Serial.print(F(" rssi=")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm  [ok]"));
    } else {
        Serial.print(F("OFFLINE  ssid='")); Serial.print(_cfg->getWifiSsid()); Serial.println(F("'"));
        Serial.println(F("  [!! verifica SSID/pass con 'show'; hotspot acceso e a 2.4 GHz; poi 'wifi connect']"));
    }

    // --- NTP / ora (serve alla TLS) ---
    time_t now = time(nullptr);
    Serial.print(F("[NTP] epoch=")); Serial.print((unsigned long)now);
    if (now >= (time_t)1700000000UL) Serial.println(F("  [ok ora valida]"));
    else Serial.println(F("  [!! ora NON sincronizzata -> la TLS verso HiveMQ fallisce; serve internet sull'hotspot]"));

    // --- MQTT ---
    Serial.print(F("[MQTT] "));
    if (!_mqtt->isConfigured()) {
        Serial.println(F("NON CONFIGURATO  [imposta 'set mqtt_broker/user/pass' + 'save' + 'reboot']"));
    } else if (_mqtt->isConnected()) {
        Serial.print(F("CONNESSO ")); Serial.print(_cfg->getMqttBroker());
        Serial.print(':'); Serial.print(_cfg->getMqttPort()); Serial.println(F("  [ok]"));
    } else {
        Serial.print(F("DISCONNESSO ")); Serial.print(_cfg->getMqttBroker());
        Serial.print(':'); Serial.println(_cfg->getMqttPort());
        if (wifiUp) Serial.println(F("  [!! WiFi ok ma MQTT giu' -> ora/TLS/credenziali: controlla [NTP] sopra, user/pass, URL broker]"));
        else        Serial.println(F("  [in attesa del Wi-Fi]"));
    }

    // --- Link seriale col Mega ---
    Serial.print(F("[MEGA link] "));
    if (_logic->isMegaConnected()) {
        Serial.print(F("OK (ultimo msg ")); Serial.print(_logic->megaLastMessageAgeMs() / 1000UL);
        Serial.println(F(" s fa)  [ok]"));
    } else {
        Serial.println(F("ASSENTE  [!! nessun frame dal Mega]"));
        Serial.println(F("  cablaggio: Mega TX1(D18)->partitore->RX2(GPIO16), Hub TX2(GPIO17)->RX1(D19), GND comune"));
    }

    // --- Sistema ---
    Serial.print(F("[SISTEMA] device_id=")); Serial.print(_logic->deviceId());
    Serial.print(F(" free_heap=")); Serial.print(ESP.getFreeHeap());
    Serial.print(F(" B uptime=")); Serial.print(millis() / 1000UL); Serial.println(F(" s"));
    Serial.println(F("============================================="));
}

void HubCli::printTelemetry() {
    TelemetryFast tf;
    TelemetryDeep td;
    _logic->getTelemetrySnapshot(tf, td);
    if (tf.epoch_s == 0 && tf.lux == 0 && td.uptime_s == 0 &&
        tf.top_dist_cm == 0 && tf.water_level_cm == 0) {
        Serial.println(F("[CLI] nessuna telemetria ricevuta dal Mega finora"));
        return;
    }
    Serial.println(F("--- ultima telemetria dal Mega ---"));
    Serial.print(F("top         = ")); Serial.print(tf.top_dist_cm);         Serial.println(F(" cm"));
    Serial.print(F("front_right = ")); Serial.print(tf.front_right_dist_cm); Serial.println(F(" cm"));
    Serial.print(F("front_left  = ")); Serial.print(tf.front_left_dist_cm);  Serial.println(F(" cm"));
    Serial.print(F("left        = ")); Serial.print(tf.left_dist_cm);        Serial.println(F(" cm"));
    Serial.print(F("right       = ")); Serial.print(tf.right_dist_cm);       Serial.println(F(" cm"));
    Serial.print(F("water_level = ")); Serial.print(tf.water_level_cm);      Serial.println(F(" cm"));
    Serial.print(F("soil        = ")); Serial.println(tf.soil_moisture);
    Serial.print(F("lux         = ")); Serial.println(tf.lux);
    Serial.print(F("mega_uptime = ")); Serial.print(td.uptime_s);            Serial.println(F(" s"));
    Serial.print(F("mega_ram    = ")); Serial.print(td.free_ram_bytes);      Serial.println(F(" B"));
    Serial.print(F("epoch_s     = ")); Serial.println(tf.epoch_s);
}

void HubCli::printHelp() {
    Serial.println(F("--- SmartVase Hub CLI v" HUB_FW_VERSION " ---"));
    Serial.println(F("help                      questo menu"));
    Serial.println(F("version                   versione firmware"));
    Serial.println(F("status                    Wi-Fi, MQTT, link Mega, heap"));
    Serial.println(F("diag                      diagnostica Wi-Fi/MQTT/Mega con hint"));
    Serial.println(F("show                      configurazione NVS"));
    Serial.println(F("set <chiave> <valore>     wifi_ssid|wifi_pass|mqtt_broker|"));
    Serial.println(F("                          mqtt_port|mqtt_user|mqtt_pass"));
    Serial.println(F("save                      salva config su NVS"));
    Serial.println(F("wifi connect              ritenta connessione Wi-Fi"));
    Serial.println(F("reboot                    riavvia l'Hub"));
    Serial.println(F("--- passthrough Mega (via seriale Protobuf) ---"));
    Serial.println(F("telemetry                 ultima telemetria dal Mega"));
    Serial.println(F("water <ms>                irrigazione (protez. tanica)"));
    Serial.println(F("mode <idle|light|shadow>  modalita' movimento"));
    Serial.println(F("stop                      ferma motori e pompa"));
    Serial.println(F("soil                      umidita' suolo"));
    Serial.println(F("megadiag                  chiede una TelemetryDeep al Mega"));
    Serial.println(F("megareset                 soft reset del Mega"));
}
