# SmartVase — Stato del firmware (Mega + ESP32 Hub + ESP32-CAM)

> **Scope di questo documento.** Sintesi operativa dello stato dei **tre
> firmware** del progetto (Arduino Mega, ESP32 Hub, ESP32-CAM) e foglio
> di lavoro dei TODO che vedo aperti. È un *companion* di
> [`docs/ARCHITECTURE.md`](ARCHITECTURE.md), che resta il riferimento di
> architettura completo (cloud, vision, app inclusi). Qui non si parla di
> cloud / vision Python / app Android se non strettamente necessario.
>
> Aggiornato al **2026-06-11** (sera pre-bring-up).

---

## 0. Aggiornamento 2026-06-11 — hardening pre-laboratorio

Repo riallineato a `origin/main` (i rename in staging sono stati risolti,
`docs/PINS - Sheet1.csv` e questo documento sono committati). Le tre
build PlatformIO **compilano tutte** in locale, offline:

| Target | Esito | RAM | Flash |
|--------|-------|-----|-------|
| Mega v5.1   | SUCCESS | 50.1% | 16.4% |
| Hub v1.2    | SUCCESS | 14.3% | 74.2% |
| CAM v2.1    | SUCCESS | 15.9% | 32.9% |

Interventi principali (dettaglio nelle sezioni e nei commit):

- **Mega v5.1** — protezione pompa su tanica vuota/US4 guasto (richiesta
  esplicita: soglia `tank_empty_cm` in EEPROM, default 20 cm, taratura da
  CLI `tank <cm>`; blocco su `water`/`pump` e auto-stop durante
  l'irrigazione); modalità `standalone` per i test a banco senza Hub
  (sospende il deadman); driver locali `Ultrasonic` (al posto della lib
  enjoyneering che bloccava 50 ms a lettura — la `begin()` peraltro non
  veniva mai chiamata: nessuna sonda avrebbe letto) e `RtcDs3232` (la lib
  DS3232RTC non è installabile: macchina offline verso il registry);
  `rtc set <epoch>` da CLI; BME680 dietro flag `BME680_ENABLED 0` (non
  montato, confermato 2026-06-11); fix `bme_read_errors` che cresceva a
  ogni TelemetryDeep anche senza sensore; reset del backoff anti-stuck
  dopo 60 s di marcia pulita; init relè senza glitch.
- **Hub v1.2** — **fix crash latente**: i manager globali copiavano gli
  handle delle code FreeRTOS quando erano ancora NULL (static init); ora
  sono creati in `setup()` dopo `xQueueCreate`. **Heartbeat periodico
  Hub→Mega ogni 30 s** (senza, il deadman del Mega scattava anche con
  l'Hub collegato). CLI seriale completa (`HubCli`): provisioning Wi-Fi/MQTT
  su NVS + comandi passthrough verso il Mega (`water`, `mode`, `soil`,
  `diag`, `telemetry`, ...) per testare la catena seriale senza rete.
  `WifiManager` non cancella più le credenziali su timeout e non avvia
  più l'AP automaticamente: boot deterministico offline. MQTT silente se
  non configurato. Telemetria de-duplicata (publish su TelemetryDeep,
  timer solo come fallback). Echo `[ACK Mega]` su seriale.
- **CAM v2.1** — fix **errore di compilazione** (doppia dichiarazione di
  `t0` in `connectWifi`: la v2.0 non aveva mai compilato) e **use-after-free**
  (`fb->len` letto dopo `esp_camera_fb_return`). Wi-Fi non bloccante con
  retry ogni 30 s (prima il loop si bloccava 30 s a tentativo). CLI
  seriale (provisioning NVS + `capture` di test senza upload + `stats`).
  Cattura automatica solo a catena completa configurata; metriche di
  upload contate solo su tentativi reali.
- **Build system** — `build_*.bat` puntavano a un profilo utente
  inesistente (`C:\Users\Giacomo Radin\...`): ora usano `%USERPROFILE%`
  e accettano argomenti extra (`build_mega.bat -t upload`). Seedata
  PubSubClient nella cache libdeps della CAM (registry irraggiungibile).
- **Docs** — nuova [`docs/Lab_Bringup_Checklist.md`](Lab_Bringup_Checklist.md):
  procedura passo-passo per il collaudo di domani (ordine sicuro, tabella
  troubleshooting, taratura tanica). README del Mega allineato alla v5.1.

Decisioni hardware confermate da Giacomo il 2026-06-11: **solo RTC DS3232
montato** (niente BME680, niente partitore batteria), polarità relè da
verificare a banco, profondità tanica ignota (soglia configurabile),
laboratorio **senza rete** (debug solo via CLI seriali), provisioning
credenziali via CLI seriale.

---

## 1. Stato delle tre versioni firmware

| Modulo                   | Versione | Sorgenti                                                | Build |
|--------------------------|----------|---------------------------------------------------------|-------|
| Platform Controller Mega | **v5.1** | `firmware/2_platform-controller_mega/.../src/`          | ✅ SUCCESS (offline) |
| ESP32 Hub                | **v1.2** | `firmware/1_esp32-hub/.../src/` + `include/`            | ✅ SUCCESS (offline) |
| ESP32-CAM                | **v2.1** | `firmware/3_esp32-cam/.../src/main.cpp`                 | ✅ SUCCESS (offline) |
| Protocollo nanopb        | **v4.0** | `firmware/.../src/smartvase.proto` + `infra/smartvase-proto/` | invariato |

Nessun firmware è ancora stato **flashato** sull'hardware reale: il
collaudo è pianificato per il 2026-06-12 seguendo
[`docs/Lab_Bringup_Checklist.md`](Lab_Bringup_Checklist.md).

---

## 2. Arduino Mega — Platform Controller v5.0

**Path**: `firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/`

### 2.1 Moduli (un file = una responsabilità)

| File                                                                                 | Ruolo                                                                                  |
|--------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------|
| [`src/main.cpp`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/main.cpp) | Setup + loop non bloccante, WDT 4 s, scheduler telemetria/heartbeat/log, health checks |
| [`src/Movement.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Movement.cpp) | State machine motori + light/shadow seeking + avoidance + STUCK backoff                |
| [`src/Sensors.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Sensors.cpp) | 6 HC-SR04 round-robin con EMA, ADC (lux + soil), BME680, RTC DS3232                    |
| [`src/Communication.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Communication.cpp) | Framing seriale verso Hub + parser RX a stati + dispatch Command → side-effects        |
| [`src/Persistence.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Persistence.cpp) | EEPROM dual-slot (64 / 128 byte) + magic + CRC16 + write throttling                    |
| [`src/Pump.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Pump.cpp) | Relè D10 active-low non-bloccante, durata max 60 s, aggiorna stats                     |
| [`src/Cli.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Cli.cpp) | CLI debug su USB (`help`, `status`, `stats`, `sensors`, `mode`, `motor`, `pump`, `reboot`) |
| [`src/SystemStatus.h`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/SystemStatus.h) | Struct condivisa: degradedMode, deviceId, flag richiesta reset, ecc.                   |
| [`src/Crc16.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Crc16.cpp) | CRC16-CCITT (poly 0x1021) condiviso fra Communication e Persistence                    |

### 2.2 PIN map allineato al CSV (`docs/PINS - Sheet1.csv`)

| Periferica                | Pin Mega                       | Note                                         |
|---------------------------|--------------------------------|----------------------------------------------|
| US1 — frontale alto       | TRIG 33 / ECHO 35              | navigazione                                  |
| US2 — frontale destro     | TRIG 26 / ECHO 27              | navigazione                                  |
| US3 — frontale sinistro   | TRIG 36 / ECHO 37              | navigazione                                  |
| US4 — tanica acqua        | TRIG 50 / ECHO 51              | livello acqua serbatoio                      |
| US5 — laterale sinistro   | TRIG 4  / ECHO 5               | navigazione                                  |
| US6 — laterale destro     | TRIG 28 / ECHO 29              | navigazione                                  |
| Forcella umidità suolo    | A0                             | ADC grezzo 0..1023                           |
| Fotoresistore             | A1                             | ADC grezzo 0..1023 (spostato da A0)          |
| Batteria (partitore)      | A2                             | `BATTERY_MONITORING_ENABLED = 0` di default  |
| Motore L (H-bridge)       | ENA 6 (PWM), IN1 43, IN2 45    |                                              |
| Motore R (H-bridge)       | ENB 7 (PWM), IN3 47, IN4 49    |                                              |
| Relè pompa                | D10 (IN1) — active LOW         | + D11 riservato, idle high                   |
| Relè backup               | D11                            | non usato dal firmware                       |
| RTC DS3232                | SDA 20 / SCL 21 (I²C 0x68)     |                                              |
| BME680                    | I²C 0x76                       | **non presente nel CSV** — vedi §6           |
| UART verso Hub            | Serial1 — D18(TX1) / D19(RX1)  | 115200 baud                                  |
| USB / CLI debug           | Serial — D0(RX) / D1(TX)       | 115200 baud                                  |

### 2.3 Loop principale (`main.cpp`)

```
wdt_reset()
cli.tick()                                  # CLI USB
comm.handleSerial()                         # RX framing + dispatch Command
sensors.sampleSensors()                     # round-robin: 1 sonda US ogni 30 ms + ADC
pump.tick()                                 # auto-off su timer
movement.handleMovementSM(obstacleView, lux, config, stats, degraded)
if (now - lastFastTelemetryMs  >= 1000 ms) sendFastTelemetry()
if (now - lastDeepTelemetryMs  >= 30 s)    sendDeepTelemetry()
if (now - lastHeartbeatMs      >= 5 s)     sendHeartbeat()
if (now - lastLogDrainMs       >= 200 ms)  drainLogQueue()
persistence.saveStats(false)                # throttled internamente
health checks: SRAM hysteresis 800/1200 + hub deadman 120 s
if (softResetRequested) performSoftReset()  # via WDT_15MS
```

### 2.4 Resilienza & osservabilità (concetti chiave)

- **WDT 4 s**: ogni stallo > 4 s causa reset, contato in `watchdog_resets`.
- **MCUSR mirror** in `.init3`: distingue power-on da watchdog reset al boot.
- **Hysteresis SRAM**: degraded mode entra sotto 800 B liberi, esce sopra
  1200 B — niente oscillazione attorno alla soglia.
- **Hub deadman**: se Hub muto > 120 s → degraded mode (`hub_missing`),
  motori fermi, pompa off.
- **Log queue circolare** (20 slot) con `noInterrupts()` solo sul producer
  enqueue. Overflow contato in stats.
- **EEPROM dual-slot** con magic number + CRC16, rotazione slot a ogni
  scrittura (wear leveling), throttling 60 s (config) / 300 s (stats).
- **Framing seriale** SOF=`0xAA`, length 2 byte big-endian, payload protobuf,
  CRC16-CCITT (poly `0x1021`). Errori contati in `pb_decode_failures`.
- **Safety pompa**: hard cap 60 s per ciclo, rifiuto se già attiva o se in
  degraded mode.
- **Safety motori**: timeout 20 s di attività ininterrotta → stop forzato.

### 2.5 State machine `Movement`

```
M_IDLE ───────► M_MOVING ───────► M_AVOID_START ───► M_AVOID_REVERSING ───► M_AVOID_TURNING
   ▲                ▲                                                              │
   │                │                                                              ▼
   │                └──────────────────── front clear ◄─────────────────────── M_AVOID_TURNING
   │                                                                              │
   └────────────────── backoff scaduto ◄──── M_STUCK ◄── 3 tentativi falliti ─────┘
```

- Soglia ostacolo frontale: 20 cm (US1 ∪ US2 ∪ US3). Laterale: 12 cm (US5/US6).
- Direzione di rotazione in avoidance: preferisce il lato libero; random se
  entrambi liberi o entrambi occupati.
- Stuck backoff: parte da 30 s, +10 s a ogni rientro in `M_STUCK`.

### 2.6 Comandi gestiti (Hub → Mega)

`water` · `setMode {IDLE, LIGHT, SHADOW}` · `stop` · `requestDiagnostics`
(restituisce un `TelemetryDeep`) · `setMotionParams {reverse_ms, turn_ms}`
(persistito in EEPROM) · `readSoil` (ritorna ADC corrente nel campo
`value` della `CommandResponse`) · `softReset`.

---

## 3. ESP32 Hub — Logic & Web Hub v1.0

**Path**: `firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/`

### 3.1 Architettura FreeRTOS

3 task pinnati ai due core + 4 code di comunicazione.

| Task              | Core | Prio   | Stack | Ruolo                                                       |
|-------------------|------|--------|-------|-------------------------------------------------------------|
| `TaskSerialMega`  | 1    | 3 alta | 4 KB  | UART2 ↔ Mega, encode/decode protobuf + framing CRC16        |
| `TaskMqttLink`    | 0    | 2 med  | 8 KB  | PubSubClient TLS verso HiveMQ Cloud, reconnect 5 s          |
| `TaskMainLogic`   | 0    | 1 bass | 8 KB  | Bridge JSON ↔ Protobuf, timer telemetria, deadman switch    |
| `loop()` (arduino)| —    | idle   | —     | placeholder per CLI (oggi solo `vTaskDelay(100)`)           |

Code (`xQueueCreate`, capienza 10):
- `serialRxQueue` Mega → MainLogic — `SerialMessage { WrapperMessage }`
- `serialTxQueue` MainLogic → Mega — idem in uscita
- `mqttTxQueue`   MainLogic → Mqtt  — `MqttMessage { topic[64], payload[512] }`
- `mqttRxQueue`   Mqtt → MainLogic  — `MqttCommand { ts, topic[64], payload[512] }`

### 3.2 Moduli

| File                                                                                                                                                | Ruolo                                                                                                              |
|-----------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| [`src/main.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/main.cpp)                                            | Bootstrap NVS + Wi-Fi + queue + task                                                                               |
| [`src/ConfigManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/ConfigManager.cpp)                          | Wrapper Preferences/NVS per `DeviceConfig` (Wi-Fi + MQTT + webhook), magic + CRC16                                 |
| [`src/WifiManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/WifiManager.cpp)                              | STA con timeout; fallback **AP di provisioning** su porta 80 (`ESPAsyncWebServer`) se SSID vuoto o connessione fallita |
| [`src/SerialManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/SerialManager.cpp)                          | Serial2 (`MEGA_RX_PIN=16, MEGA_TX_PIN=17`) ↔ Mega: parser framing + encode WrapperMessage                          |
| [`src/MQTTManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/MQTTManager.cpp)                              | `WiFiClientSecure + PubSubClient`, CA HiveMQ in `include/hivemq_ca_cert.h`, LWT `offline` su `/status`, retain      |
| [`src/MainLogic.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/MainLogic.cpp)                                  | Cuore: cache last fast/deep telemetry, timer pubblicazione 60 s, deadman Mega 130 s, JSON ⇄ Protobuf, ack comandi  |
| [`include/hivemq_ca_cert.h`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/include/hivemq_ca_cert.h)                    | CA cert per HiveMQ Cloud (condiviso con la CAM via `infra/hivemq_ca_cert.h`)                                       |

### 3.3 Topic MQTT pubblicati / sottoscritti

`{id} = HUB_<ultimi 3 byte MAC>` (es. `HUB_A1B2C3`).

| Topic                              | Direzione         | Payload                                                                    |
|------------------------------------|-------------------|----------------------------------------------------------------------------|
| `smartvase/{id}/telemetry`         | Hub → Cloud       | JSON con campi `distances_cm{top,front_right,front_left,left,right}` + ambient + counters |
| `smartvase/{id}/logs`              | Hub → Cloud       | JSON `{timestamp_ms, level, event, detail, source_device}`                 |
| `smartvase/{id}/alarm`             | Hub → Cloud       | JSON `{type, detail}` (es. `mega_offline`, `tx_queue_full`)                |
| `smartvase/{id}/status`            | Hub → Cloud       | `online` / `offline` (LWT, retained)                                       |
| `smartvase/{id}/command/#`         | Cloud → Hub       | JSON `{type, cmd_id, …}` — mapping in `processMqttCommand`                 |
| `smartvase/{id}/command/ack`       | Hub → Cloud       | JSON `{cmd_id, status, detail, value, exec_time_ms}`                       |

Tipi comando mappati Hub → Mega: `setMode | water | stop | requestDiagnostics |
setMotionParams | readSoil | softReset`. Comandi non riconosciuti → alarm
`unknown_command_type`.

### 3.4 Flusso telemetria

1. Mega manda `TelemetryFast` ogni 1 s e `TelemetryDeep` ogni 30 s su Serial1.
2. Hub aggiorna `_lastFastTelemetry` e `_lastDeepTelemetry` in `processSerialMessage`.
3. Pubblicazione MQTT:
   - **Immediata** all'arrivo della `TelemetryDeep` (composito fast + deep);
   - **Periodica** ogni 60 s via `telemetryTimerCallback` (stesso composito).
4. `Heartbeat` solo aggiorna `_lastMegaHeartbeatMs` (niente pubblicazione).
5. Se Mega muto > 130 s → publish `alarm` con `type=mega_offline`. Quando
   torna: `alarm` con `type=mega_online`. 130 s = 120 s deadman Mega + 10 s
   margine rete/seriale.

---

## 4. ESP32-CAM — Vision Co-Processor v2.0

**Path**: `firmware/3_esp32-cam/Radin_Giacomo_SmartVase_VisionCoProcessor_ESP32CAM/`

Refactor totale da bench-code (v14, dumpava i frame su Serial) a firmware
autonomo Wi-Fi/MQTT/Cloud.

### 4.1 Loop di vita

```
setup():
   disabilita brown-out
   loadConfig() / loadStats() da NVS
   makeDeviceId() = "CAM_" + ultimi 3 byte MAC
   initCamera() OV2640: SVGA (con PSRAM) / VGA (senza), JPEG quality 12
   connectWifi() + NTP (pool.ntp.org, attesa max 3 s)
   mqttInit() + mqttReconnect()

loop():
   se WiFi giù → riconnetti
   se MQTT giù → riconnetti; mqttClient.loop()
   ogni cfg.interval_s (default 300 s):
      esp_camera_fb_get()
      crc32_le(buffer)
      uploadJpeg() → POST multipart streaming a cfg.upload_url
                     (Cloud Function risponde con {image_url})
      esp_camera_fb_return()
      publishVisionImage(url, size, crc, capture_time)
      saveStats() in NVS
```

### 4.2 Config (NVS namespace `cam`)

| Chiave        | Tipo   | Default                                    | Note                                  |
|---------------|--------|--------------------------------------------|---------------------------------------|
| `wifi_ssid`   | string | (vuoto)                                    | da scrivere a mano la prima volta     |
| `wifi_pass`   | string | (vuoto)                                    |                                       |
| `mqtt_broker` | string | (vuoto)                                    | `<id>.s1.eu.hivemq.cloud`             |
| `mqtt_port`   | uint16 | 8883                                       | TLS                                   |
| `mqtt_user`   | string | (vuoto)                                    |                                       |
| `mqtt_pass`   | string | (vuoto)                                    |                                       |
| `upload_url`  | string | (vuoto)                                    | endpoint Cloud Function `upload-image`|
| `interval_s`  | uint32 | 300                                        | secondi tra una cattura e l'altra     |

Stats persistenti (namespace `cam_stats`): `succ_frames`, `fail_frames`,
`upload_err`, `mqtt_err`, `total_cap_ms`.

### 4.3 Topic MQTT

`{id} = CAM_<ultimi 3 byte MAC>`.

| Topic                              | Direzione   | Payload                                                    |
|------------------------------------|-------------|------------------------------------------------------------|
| `smartvase/{id}/vision/image`      | CAM → Cloud | JSON `{timestamp_utc, device_id, image_url, size_bytes, crc32, capture_time_ms, content_type}` |
| `smartvase/{id}/vision/status`     | CAM → Cloud | `online` / `offline` (LWT, retained)                       |
| `smartvase/{id}/vision/command/#`  | Cloud → CAM | JSON `{type}` — supportati: `captureNow`, `reboot`         |

### 4.4 Dettagli implementativi notevoli

- **Upload streaming**: il body multipart non è mai allocato in heap. Una
  classe `MultipartStream` interna concatena header + buffer JPEG + footer
  on-the-fly mentre `HTTPClient::sendRequest("POST", Stream*, total)` lo
  legge a chunk. Utile dato lo heap ridotto della ESP32-CAM e dimensioni
  SVGA tipiche di 20–80 KB.
- **TLS verso Cloud Function**: oggi `client.setInsecure()`. Marcato TODO
  nel codice — vedi §6.
- **TLS verso HiveMQ**: cert CA hardcoded (`infra/hivemq_ca_cert.h`), validato.
- **NTP best-effort**: 3 s di timeout in `connectWifi`. Se NTP fallisce, il
  `timestamp_utc` finisce a 0/garbage.

---

## 5. Protocollo seriale Hub ↔ Mega (`smartvase.proto` v4.0)

File canonico: [`infra/smartvase-proto/smartvase.proto`](../infra/smartvase-proto/smartvase.proto)
(copia identica anche in `firmware/.../src/smartvase.proto` e
`firmware/.../include/smartvase.proto`).

### 5.1 Messaggi

- **`TelemetryFast`** — 5 distanze navigazione (`top, front_right, front_left,
  left, right`) + `water_level_cm` + `soil_moisture` (ADC) + `lux` (ADC) +
  `movement_state` + `epoch_s` (RTC) + `device_id`.
- **`TelemetryDeep`** — BME680 (`temperature_c, humidity_percent, pressure_hpa,
  gas_resistance_ohms`) + `uptime_s, free_ram_bytes, epoch_s` + 9 contatori
  cumulativi + `battery_voltage` (riportato 0 se monitoring off) + `device_id`.
- **`Log`** — `LogLevel ∈ {INFO, WARN, ERROR, CRITICAL}`, `event[24]`,
  `detail[32]`, `timestamp_ms`, `source_device[16]`.
- **`Heartbeat`** — `uptime_s, is_degraded, device_id`.
- **`Command`** (oneof): `water | set_mode | stop | request_diagnostics |
  set_motion_params | read_soil | soft_reset`, `cmd_id = 99`.
- **`CommandResponse`** — `status ∈ {OK, ERROR}`, `detail[64]`, **`value` (int32)**
  per risposte con payload numerico (es. `readSoil` ritorna il valore ADC),
  `cmd_id`, `exec_time_ms`.

### 5.2 Framing seriale

```
0xAA | len_hi | len_lo | payload (protobuf, ≤256 B) | crc16_hi | crc16_lo
```

CRC16-CCITT, poly `0x1021`, init `0x0000`. Parser RX a 6 stati.
Errori → `stats.pb_decode_failures++` e drop del frame.

### 5.3 Codegen nanopb

Script `infra/smartvase-proto/generate_proto.bat`. Vincolo Arduino IDE:
nel `.pb.h` generato bisogna sostituire `#include <pb.h>` → `#include "pb.h"`.

---

## 6. Build & dipendenze

Wrapper PlatformIO alla root del repo: `build_mega.bat`, `build_hub.bat`,
`build_cam.bat`. Usano la `pio.exe` in `C:\Users\Giacomo Radin\.platformio\penv\Scripts\`.

### 6.1 `lib_deps`

| Env                | Librerie                                                                                                  |
|--------------------|-----------------------------------------------------------------------------------------------------------|
| `megaatmega2560`   | `adafruit/Adafruit BME680 Library@^2.0.1`, `enjoyneering/HCSR04@^1.1.0`, `paulstoffregen/Time@^1.6.1`, `jchristensen/DS3232RTC@^2.0.1` |
| `esp32dev` (Hub)   | `ESP32Async/AsyncTCP@^3.3.2`, `ESP32Async/ESPAsyncWebServer@^3.6.0`, `bblanchon/ArduinoJson@^6.19.4`, `knolleary/PubSubClient@^2.8` |
| `esp32cam`         | `bblanchon/ArduinoJson@^6.19.4`, `knolleary/PubSubClient@^2.8` (+ `BOARD_HAS_PSRAM`, `-mfix-esp32-psram-cache-issue`) |

Note:
- Il Mega **non ha più** dipendenza da nessuna lib di EEPROM/WString
  esterna (sta in core Arduino).
- Il Hub **dichiara** AsyncTCP + ESPAsyncWebServer (li usa `WifiManager`
  per l'AP di provisioning). Non c'è ancora una vera web UI utente.
- La CAM non dichiara `esp_camera` perché è parte del core Arduino-ESP32
  (board `esp32cam`).

### 6.2 Discrepanze documentazione vecchia

[`firmware/2_platform-controller_mega/README_PlatformController_Arduino.md`](../firmware/2_platform-controller_mega/README_PlatformController_Arduino.md)
è ancora **pre-refactor** (parla di v3, `smartvase_v3.proto`, "HCSR04 by
gamegine" invece di `enjoyneering/HCSR04`, manca DS3232RTC, manca CLI
estesa). Da riallineare alla v5 oppure cancellare in favore di ARCHITECTURE.md.

---

## 7. Cosa manca o da sistemare — TODO firmware

> **Nota 2026-06-11**: molti item di questa sezione sono stati chiusi con
> l'hardening pre-bring-up (vedi §0): build dei 3 target ✅, BME680
> chiarito (assente, dietro flag) ✅, batteria flag-off confermato ✅,
> provisioning CLI Hub+CAM ✅, bug `t0` CAM ✅, use-after-free fb ✅,
> Wi-Fi CAM non bloccante ✅, metriche upload ✅, doppia pubblicazione
> telemetria Hub ✅, backoff anti-stuck con reset ✅, protezione pompa
> tanica-vuota ✅, deadman a banco (standalone) ✅, heartbeat Hub→Mega ✅.
> Restano aperti: flash/validazione su HW (domani), TLS pinning upload CAM,
> vision/result nel Hub, automazione autonoma, OTA, rate-limit comandi,
> consolidamento copie proto e cert.

Tag: **[BLK]** = bloccante per portare il robot online · **[FUNC]** = feature
da completare · **[POL]** = polish / qualità / sicurezza.

### 7.1 Bring-up sul nuovo prototipo

- **[BLK]** Build & flash dei tre firmware sull'HW reale.
  Mai fatto dopo il refactor v5: aspettati 1–2 giri di fix lib_deps /
  include path / nomi simboli HCSR04 a seconda della versione installata.
- **[BLK]** Validare PIN map sul banco: in particolare quale ENA corrisponde
  al motore fisicamente "sinistro" (vedi nota in §4.2 di ARCHITECTURE.md).
- **[BLK]** Cablare e configurare CAM via NVS (`wifi_ssid`, `wifi_pass`,
  `mqtt_*`, `upload_url`): senza queste chiavi la CAM non parte. **Manca un
  meccanismo di provisioning** (oggi vanno scritte da host via tool Preferences
  o sketch usa-e-getta).
- **[BLK]** Verificare presenza fisica del BME680: è usato dal firmware via
  I²C 0x76 ma **non è elencato nel CSV** dei pin. Se non è più nel BOM, va
  rimosso dal firmware (oppure stub-bato con `bme_status=false` permanente).

### 7.2 Mega — gap noti

- **[FUNC]** Batteria: `BATTERY_MONITORING_ENABLED=0` ⇒ il campo
  `battery_voltage` in `TelemetryDeep` è sempre 0. Quando si cabla il
  partitore (R1=30k, R2=7.5k su A2) basta passare il flag a 1; eventualmente
  ricalibrare il rapporto e/o spostare il pin se A2 va riassegnato.
- **[FUNC]** Statistiche `light_seeking_sessions` e `shadow_seeking_sessions`
  vengono incrementate in `Movement` ma **non sono incluse** in `TelemetryDeep`
  né stampate dalla CLI `stats`. Aggiungere o rimuoverle.
- **[FUNC]** Stesso discorso per `escape_attempts`: contata ma mai pubblicata.
- **[POL]** `Movement::handleMovementSM`: lo stuck backoff cresce di +10 s
  per evento e **non si resetta mai** (anche dopo un periodo lungo di
  navigazione pulita). Dopo molti stuck il robot dorme per minuti.
  Suggerimento: dimezzare/resettare dopo N secondi di `M_MOVING` senza eventi.
- **[POL]** Light/shadow seeking è una sterzata "binaria" continua finché
  la luce non passa la soglia. Manca:
  - timeout / numero massimo di sterzate prima di passare a `IDLE`;
  - misura del *gradiente* di luce (oggi sterza sempre nella stessa direzione,
    può girare in tondo).
- **[POL]** `ReadSoilCommand` restituisce **una** singola lettura ADC, senza
  filtro né conversione a unità calibrate. Per uso in app probabilmente
  serve già il valore EMA / un range mappato.
- **[POL]** `Communication::executeCommand`: nessun rate-limit. Un client
  malizioso può saturare la pompa con tanti `water` consecutivi entro i 60 s
  (anche se la `start()` rifiuta se già attiva, copre solo concurrent).
- **[POL]** Persistence: i due slot config sono distanti 64 byte e gli stats
  256 byte; sizeof attuale di `DeviceConfig` è ~14 byte e `CumulativeStats`
  ~60 byte. Margine ampio ma se cresci aggiungendo campi attento a non
  sovrapporli.
- **[POL]** Default `light_threshold` in `Persistence.cpp`: serve riprenderlo
  e verificare che abbia senso col fotoresistore reale (oggi è inizializzato
  nel costruttore di `Movement` a 600 e poi sovrascritto dalla config: ridondante).

### 7.3 Hub — gap noti

- **[BLK]** **Non c'è sottoscrizione a `smartvase/{id}/vision/result`** —
  i risultati della pipeline vision non rientrano nel Hub e quindi non
  possono mai influenzare il comportamento del robot. Il documento di
  architettura prevede questo loop chiuso, il codice no.
- **[FUNC]** Nessuna **automazione autonoma** (nessun `applyDefaultPlantLogic`):
  il robot innaffia solo se l'app manda un `water`, e non c'è scelta
  automatica di `LIGHT`/`SHADOW` in base a sensori. Decisione di design da
  riprendere — vedi domande in fondo.
- **[FUNC]** `loop()` del task Arduino è di fatto vuoto (`handleCLI()` è un
  commento). Niente CLI di debug sul Hub via USB, mentre il Mega ce l'ha.
- **[FUNC]** `processMqttCommand`: non valida che il topic effettivo del
  comando sia coerente con il `type` nel JSON. Un payload `{"type":"water"}`
  pubblicato su `smartvase/.../command/setMode` viene comunque eseguito.
- **[POL]** Pubblicazione telemetria: avviene sia su arrivo della `TelemetryDeep`
  (ogni 30 s) sia dal timer (ogni 60 s). In pratica due pubblicazioni quasi
  identiche entro 30 s. O togli il timer o sganci la pubblicazione su `deep`.
- **[POL]** Le distanze (cambiano molto più rapidamente delle altre metriche)
  sono pubblicate solo con la cadenza della deep / del timer. Per il
  comportamento app in tempo reale serve un topic dedicato a frequenza più
  alta (es. `telemetry/fast` ogni 1–2 s con solo distances + state).
- **[POL]** `MQTTManager::reconnect`: backoff fisso 5 s, niente jitter. Sotto
  blackout di rete fa polling regolare e prevedibile — accettabile, da tenere
  d'occhio.
- **[POL]** `WifiManager` AP di provisioning: presente ma manca una pagina
  HTML servita di default; verificare cosa restituisce l'`ESPAsyncWebServer`.
  Non ho letto il file completo.

### 7.4 ESP32-CAM — gap noti

- **[BLK]** Provisioning NVS: nessuno strumento per scrivere `wifi_*`,
  `mqtt_*`, `upload_url` la prima volta. Servono o (a) uno sketch usa-e-getta
  che fa `prefs.putString(...)`, o (b) un AP di provisioning come quello del
  Hub, o (c) hard-code temporaneo in `setup()` durante bring-up.
- **[BLK]** `uploadJpeg()` usa `WiFiClientSecure::setInsecure()` verso la
  Cloud Function: TLS non validato. Da pinnare il cert/CA dell'endpoint
  prima di mettere il robot fuori casa (decisione condivisa con Fia).
- **[FUNC]** I comandi MQTT supportati sono solo `captureNow` e `reboot`.
  Mancano: `setInterval(s)`, `setQuality(0..63)`, `getStats`, `flashOn/Off`.
- **[FUNC]** Coordinamento Hub ↔ CAM: il Hub non chiede mai un capture; la
  CAM cattura solo a interval timer. Per le condizioni interessanti
  ("appena finito di muoversi", "luce stabile") serve un comando dal Hub.
- **[POL]** `loop()` chiama `delay(50)` invece di gestire timer in modo
  più reattivo, e `mqttClient.loop()` può bloccare alcuni ms — accettabile.
- **[POL]** Se `cfg.upload_url` è vuoto, ogni ciclo conta un `upload_err`
  ma `successful_frames` viene comunque incrementato (la cattura è andata).
  Le metriche risultano fuorvianti.
- **[POL]** Doppio `unsigned long t0` in `connectWifi()`: c'è un bug di
  shadowing (la seconda dichiarazione viene dopo la prima `while`).
  Da verificare se compila o se va già pulito.

### 7.5 Trasversali ai tre firmware

- **[FUNC]** **OTA assente** ovunque. Critico se il robot finisce in zone
  dove il flash via USB è scomodo.
- **[FUNC]** Niente versionatura runtime: ogni firmware ha la sua "v5.0/v1.0/v2.0"
  in commento ma non la pubblica in telemetria/log. Aggiungere `fw_version`
  a `TelemetryDeep` e al payload `vision/image`.
- **[POL]** Il proto ha 3 copie a disco (Mega, Hub, infra). Oggi
  `generate_proto.bat` aggiorna `infra/smartvase-proto/` e *probabilmente*
  va copiato a mano nelle altre due. Verificare e centralizzare (single
  source + simlink/copy step nel `.bat`).
- **[POL]** Hub e CAM condividono `infra/hivemq_ca_cert.h` ✅. Ma esistono
  anche copie locali: `firmware/1_esp32-hub/.../include/hivemq_ca_cert.h`
  e `firmware/3_esp32-cam/.../src/hivemq_ca_cert.h`. Da consolidare con un
  `extra_scripts` PlatformIO che le copi al build, oppure include path
  relativo a `infra/`.

---

## 8. Aree non coperte da questo documento

Per non duplicare ARCHITECTURE.md:

- **Cloud pipeline** (HiveMQ ⇄ Cloud Functions ⇄ Firestore ⇄ App/Vision):
  vedi `docs/SmartVase_data_structure.md` e `infra/cloud-functions/`.
- **Vision Python** (`vision/`): pipeline + quality_gate + leaf_health già
  con 18 test pytest passanti. Non oggetto di questo doc.
- **App Android** (Francesco): fuori dal repo.
- **Server scripts** (`server/mqtt_listener.py`): bridge dev.

---

## 9. Come orientarsi se si torna sul codice domani

1. **Aprire ARCHITECTURE.md** per il quadro complessivo.
2. **Aprire questo file** per la situazione del solo firmware e i TODO.
3. **PIN map**: `docs/PINS - Sheet1.csv` è la verità sui cablaggi.
4. **Build**: dalla root del repo, `build_mega.bat` / `build_hub.bat` /
   `build_cam.bat`.
5. **Debug Mega**: USB Serial a 115200 + comando `help`.
6. **Debug Hub**: USB Serial a 115200 (log ESP_LOG con `CORE_DEBUG_LEVEL=4`).
7. **Debug CAM**: USB Serial a 115200; per i topic MQTT sottoscrivi
   `smartvase/CAM_+/#` su un client (es. MQTT Explorer puntato a HiveMQ).

---

## 10. Allineare il working tree

Da fare prima di qualsiasi modifica al firmware:

```powershell
git status                              # verifica i rename staged
git fetch origin
git pull --ff-only origin main          # main locale è 2 commit dietro
git log --oneline -5                    # deve mostrare 83e8f4c in cima
```

Se `--ff-only` fallisce per via dei rename staged, valutare se quei rename
sono già parte di `origin/main` (lo sono, vedi §0); in tal caso
`git reset --hard origin/main` dopo aver controllato che non si perda
lavoro locale non commit-ato (oggi non c'è nulla di nuovo nel working tree
a parte i rename già duplicati).

---

_Stato di questo documento: prima stesura, 2026-05-27._
