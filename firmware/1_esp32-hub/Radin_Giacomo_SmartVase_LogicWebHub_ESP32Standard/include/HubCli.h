/*! @file HubCli.h
 *  @ingroup HubCli
 *  @brief Serial debug/provisioning/diagnostics CLI for the Hub, exposed over
 *  USB (115200 baud).
 *  @author Giacomo Radin
 *  @date 2026-06-11
 */

/*! @defgroup HubCli Serial debug CLI
 *  @brief Textual interface over USB to inspect the Hub state, provision
 *  Wi-Fi/MQTT and send test commands to the Mega, even in the lab without a
 *  network.
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
 * @brief Serial debug and provisioning CLI on the USB port (115200 baud,
 * '\\n' terminator).
 * @details Runs entirely in the Arduino loop() (lowest priority): tick()
 * reads the available characters without blocking and accumulates a line in
 * `buf` until the terminator. Locally handled commands:
 *
 *   help / ?                  command list
 *   version                   firmware version
 *   status                    Wi-Fi, MQTT, Mega link, heap, uptime
 *   show                      NVS configuration (passwords masked)
 *   set <key> <value>         wifi_ssid|wifi_pass|mqtt_broker|mqtt_port|
 *                             mqtt_user|mqtt_pass
 *   save                      persists the configuration to NVS
 *   wifi connect              retries the STA connection
 *   reboot                    reboots the ESP32
 *
 * Passthrough commands towards the Mega (they exercise the whole Protobuf
 * serial chain even without a network; the result comes back as [ACK Mega] on
 * the monitor, printed by MainLogic::processSerialMessage()):
 *
 *   telemetry                 last TelemetryFast/Deep received from the Mega
 *   water <ms>                starts irrigation (tank protection on the Mega)
 *   mode <idle|light|shadow>  changes the movement mode
 *   stop                      stops motors and pump
 *   soil                      reads the soil moisture
 *   megadiag                  requests an immediate TelemetryDeep
 *   megareset                 soft reset of the Mega
 *
 * @note All the pointers (`_cfg`, `_wifi`, `_mqtt`, `_logic`) are passed by
 * reference from `begin()` and must stay valid for the whole lifetime of the
 * object: in main.cpp they are global instances/pointers with a lifetime equal
 * to the program's.
 */
class HubCli {
public:
    /*! @brief Constructor: clears the module pointers and the line buffer
     *  (object not operational until begin() is called). */
    HubCli();

    /*! @brief Connects the CLI to the Hub modules and enables tick().
     *  @param[in] cfg NVS configuration manager, for the `show`/`set`/`save` commands.
     *  @param[in] wifi Wi-Fi manager, for `wifi connect`.
     *  @param[in] mqtt MQTT manager, for `status`/`diag`.
     *  @param[in] logic Core logic, to read telemetry/Mega link state and the
     *  device ID.
     *  @param[in] serialTxQueue Queue towards the Mega, used by sendMegaCommand()
     *  for the passthrough commands.
     *  @note To be called once in setup(), after the initialization of all the
     *  passed modules. */
    void begin(ConfigManager* cfg, WifiManager* wifi, MqttManager* mqtt,
               MainLogic* logic, QueueHandle_t serialTxQueue);

    /*! @brief To be called on every loop() iteration: reads the available
     *  characters on `Serial` without blocking, accumulates a line and executes
     *  it at the '\\n' terminator.
     *  @note No-op if begin() has not been called yet (`_cfg == nullptr`).
     *  A line longer than BUF_SIZE-1 is discarded with an error message. */
    void tick();

private:
    /*! @brief Dispatches a complete command line (NUL-terminated, without '\\n').
     *  @param[in] line Mutable buffer (some commands tokenize in-place with strtok/strchr). */
    void execute(char* line);
    /*! @brief Prints the list of available commands (`help`/`?` command). */
    void printHelp();
    /*! @brief Prints a quick status summary: Wi-Fi, MQTT, Mega link, heap, uptime (`status` command). */
    void printStatus();
    /*! @brief Prints a guided diagnostic (with hints) of Wi-Fi/NTP/MQTT/Mega link (`diag` command). */
    void printDiag();
    /*! @brief Prints the current NVS configuration, with the passwords masked (`show` command). */
    void printShow();
    /*! @brief Prints the last TelemetryFast/Deep received from the Mega, or a warning if none has arrived yet (`telemetry` command). */
    void printTelemetry();
    /*! @brief Implements the `set <key> <value>` command: updates in memory the
     *  requested configuration field (then requires `save` to persist it).
     *  @param[in,out] args String "<key> <value>"; it is modified in-place
     *  (the first space is replaced with '\\0' to separate key and value).
     *  @return true if the key is recognized and the value is syntactically valid,
     *  false otherwise (the caller prints the usage). */
    bool handleSet(char* args);
    /*! @brief Builds a Protobuf `Command` and enqueues it on `_serialTxQueue` for the Mega.
     *  @param[in] which Tag of the `command_type` oneof field (e.g. Command_water_tag).
     *  @param[in] arg Command argument (interpreted based on `which`: duration in
     *  ms for water, enum value for set_mode, ignored for commands without payload).
     *  @note Uses an auto-incrementing cmd_id starting from 9000 (`_nextCmdId`) to
     *  distinguish the responses of CLI-originated commands from the MQTT ones. */
    void sendMegaCommand(uint8_t which, uint32_t arg);

    ConfigManager* _cfg;   /**< NVS configuration manager (not owned). */
    WifiManager*   _wifi;  /**< Wi-Fi manager (not owned). */
    MqttManager*   _mqtt;  /**< MQTT manager (not owned). */
    MainLogic*     _logic; /**< Core logic (not owned). */
    QueueHandle_t  _serialTxQueue; /**< Queue towards the Mega for the passthrough commands. */
    uint32_t       _nextCmdId;     /**< Next cmd_id to assign to the commands sent from the CLI (starts at 9000). */

    static const size_t BUF_SIZE = 192; /**< Maximum capacity of a command line, including the terminator. */
    char   buf[BUF_SIZE]; /**< Accumulation buffer for the current line. */
    size_t pos;            /**< Number of characters currently accumulated in buf. */
};

/*! @} */ // end of HubCli group

#endif // HUBCLI_H
