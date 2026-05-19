# SmartVase — Architettura e contesto del progetto

> Documento di onboarding tecnico per il team SmartVase.
> Stato consolidato al **2026-05-19**. Allineato al PIN map
> `docs/PINS - Sheet1.csv` e alle decisioni architetturali correnti.

---

## 1. Stato attuale del progetto (TL;DR)

- Il prototipo hardware è in **ricostruzione attiva** (maggio 2026). Il robot
  fisico è in costruzione *ora*: il PIN map (`docs/PINS - Sheet1.csv`) è il
  riferimento autoritativo del cablaggio.
- **Refactor totale del firmware completato** in data 2026-05-19, allineato
  al nuovo PIN map + decisioni architetturali (vedi §11). Versioni:
  - Mega: **v5.0** — 6 HC-SR04, pompa via relè, RTC DS3232, modulo `Pump`
    dedicato, `Communication` ora esegue davvero i comandi end-to-end.
    Hysteresis SRAM (800/1200 B) per recovery pulito da degraded mode.
  - Hub: **v1.1** — `MainLogic.cpp` non ha più stub; pubblica telemetry,
    log, alarm e **command/ack** (topic dedicato); deadman switch attivo.
  - ESP32-CAM: **v2.0** — completamente riscritto da bench-code a
    Wi-Fi STA + NTP + MQTT TLS + upload HTTP **streaming** a Cloud Function.
  - Protocollo: **proto v4.0** — `TelemetryFast` con 5 distanze nav.
    + soil moisture + epoch_s; `CommandResponse` con campo `value`.
- **Pipeline vision Python** estesa con `metrics`, `leaf_health` (rule-based
  v0.2) e `pipeline.analyze_image()`. Output JSON conforme a
  `SmartVase_data_structure.md`. 18 test pytest passano.
- **Cloud Function stub** `upload-image` aggiunta in `infra/cloud-functions/`
  (Node 20 + Firebase Storage). Da rifinire con Fia.

---

## 2. Vision di prodotto

SmartVase è un **vaso/serra IoT mobile e autonoma**:

- Si muove su ruote alla ricerca di luce o ombra a seconda della modalità
  scelta (`LIGHT` / `SHADOW` / `IDLE`) — comportamento light-seeking /
  shadow-seeking governato dal fotoresistore.
- Innaffia la pianta su comando (pompa controllata da relè) o, in futuro,
  in autonomia in base a umidità del suolo (sensore a forcella).
- Cattura periodicamente immagini della pianta con la ESP32-CAM e ne valuta
  qualità del frame e salute fogliare via pipeline di vision Python.
- È controllato da un'**app Android** (MVVM + Compose, sviluppo di Francesco).
- Riporta telemetria e log a HiveMQ Cloud, con Firestore come store autoritativo.

Principi guida del progetto (dal README):
**Resilience & Robustness** · **Observability & Diagnostics** ·
**Performance & Efficiency** · **Modularity & Maintainability**.

---

## 3. Topologia hardware

Tre microcontrollori più un'app mobile:

| Ruolo                | MCU             | Codename     | Compito                                                                 |
|----------------------|-----------------|--------------|-------------------------------------------------------------------------|
| Platform Controller  | Arduino Mega    | *The Brawn*  | Controllo diretto di motori, pompa, sensori, RTC. Nessuna rete.         |
| Logic & Web Hub      | ESP32 standard  | *The Brain*  | Wi-Fi, MQTT/TLS verso HiveMQ, coordinamento, ponte JSON↔Protobuf.       |
| Vision Co-Processor  | ESP32-CAM       | *The Eye*    | Cattura JPEG, upload immagine, pubblicazione `vision/image` su MQTT.    |
| Android App          | —               | —            | UI utente (Kotlin, Compose, MVVM).                                      |

### 3.1 Bus di comunicazione

- **Hub ↔ Mega**: UART seriale (115200 baud). Frame:
  `SOF=0xAA | len_hi | len_lo | payload(protobuf) | crc16_hi | crc16_lo`.
  Payload = `WrapperMessage` di [smartvase.proto](infra/smartvase-proto/smartvase.proto).
- **Hub ↔ Cloud**: MQTT su TLS verso **HiveMQ Cloud**. Payload **JSON**.
- **Cloud pipeline**:
  `HiveMQ ⇄ Cloud Functions ⇄ Firestore ⇄ App Android / Vision`.
  Firestore è lo store autoritativo. Le Cloud Functions bridgiano MQTT e
  documenti Firestore (vedi [SmartVase_data_structure.md](SmartVase_data_structure.md)).
- **CAM ↔ Cloud**: **autonomo**. La ESP32-CAM si connette al Wi-Fi e
  pubblica direttamente su MQTT (o uploada su storage e poi pubblica
  `vision/image`). Il `main.cpp` attuale che stampa su Serial è *bench code*,
  non rappresenta l'architettura target.
- **Vision Python ↔ Cloud**: consuma `vision/image`, scrive `vision/result`
  (entrambi via Firestore).

---

## 4. PIN map autoritativo (Arduino Mega)

Dal file [docs/PINS - Sheet1.csv](docs/PINS - Sheet1.csv).
**Questa tabella vince su qualsiasi `#define` presente nei sorgenti
firmware attuali.** Tutto il firmware Mega va riallineato a questi pin.

### 4.1 Sensori ultrasuoni HC-SR04 (6 totali)

| ID  | Ruolo fisico                       | Trigger | Echo | Note                                   |
|-----|------------------------------------|---------|------|----------------------------------------|
| US1 | Frontale alto (avanti, in alto)    | D33     | D35  | Anti-collisione frontale superiore     |
| US2 | Frontale destro                    | D26     | D27  | Anti-collisione frontale destra        |
| US3 | Frontale sinistro                  | D36     | D37  | Anti-collisione frontale sinistra      |
| US4 | Tanica acqua                       | D50     | D51  | **Misura livello acqua nel serbatoio** |
| US5 | Lato sinistro                      | D4      | D5   | Anti-collisione laterale sinistra      |
| US6 | Lato destro                        | D28     | D29  | Anti-collisione laterale destra        |

US1, US2, US3, US5, US6 → **navigazione e obstacle avoidance** (5 sensori).
US4 → **solo** livello acqua nella tanica.

### 4.2 Driver motori (H-bridge, 2 motori DC)

Da CSV (pin del "driver motorini"):

| Funzione        | Pin Mega |
|-----------------|----------|
| Motor A — IN1   | D47      |
| Motor A — IN2   | D49      |
| Motor A — ENA   | D6 (PWM) |
| Motor B — IN3   | D43      |
| Motor B — IN4   | D45      |
| Motor B — ENB   | D7 (PWM) |

> Il firmware legacy ([Movement.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Movement.cpp))
> usa già `enA=7, in1=43, in2=45, enB=6, in3=47, in4=49`, che matcha il CSV
> a meno di un'eventuale ridefinizione di quale H-bridge è "left" vs "right".
> Da verificare a banco quale ENA corrisponde alla ruota fisica sinistra.

### 4.3 Alimentazione e RTC

| Funzione                       | Pin Mega       | Note                                                  |
|--------------------------------|----------------|-------------------------------------------------------|
| Partitore tensione batteria    | A0 (vedi nota) | Divisore R1=30k, R2=7.5k → `Vbatt = Vadc · 5.0`       |
| RTC DS3232 — SDA               | D20 (SDA)      | I²C                                                   |
| RTC DS3232 — SCL               | D21 (SCL)      | I²C                                                   |

> **Pin batteria — discrepanza aperta**: il firmware legacy usa `A2`, il CSV
> non lo elenca esplicitamente nella sezione batteria. Da confermare al
> prossimo refactor sul banco.

### 4.4 Pompa e relè

| Funzione               | Pin Mega | Note                                |
|------------------------|----------|-------------------------------------|
| Relè canale 1 (IN1)    | D10      | Pompa irrigazione                   |
| Relè canale 2 (IN2)    | D11      | Riserva / secondo attuatore         |

Il firmware Mega legacy **non** implementa ancora l'attivazione del relè:
`WaterCommand` esiste a livello Protobuf ma il side-effect HW è da scrivere.

### 4.5 Sensore umidità suolo "a forcella"

| Funzione           | Pin       | Note                                                |
|--------------------|-----------|-----------------------------------------------------|
| Forcella (signal)  | A0        | Sonda a due puntali per umidità del terreno         |
| Forcella VCC       | 5V        |                                                     |
| Forcella GND       | GND       |                                                     |

> **Conflitto fotoresistore ↔ forcella su A0**: il CSV mappa entrambi su A0,
> ma su Mega ogni ADC è single-ended. Decisione: **la forcella resta su A0,
> il fotoresistore va spostato su un altro pin analogico libero** (es. A1
> o A3). Da decidere e annotare al momento del refactor.

### 4.6 Fotoresistore (luminosità ambientale)

| Funzione           | Pin       | Note                                                |
|--------------------|-----------|-----------------------------------------------------|
| Fotoresistore (LDR)| **TBD**   | Era su A0 nel firmware legacy. Da spostare per fare posto alla forcella. |

Il fotoresistore guida la state machine `LIGHT` / `SHADOW`:
gira a destra se serve più luce, a sinistra se serve ombra.

### 4.7 Pin Mega ad oggi *non assegnati* / *non chiari*

- Sensore BME680 (T / RH / pressione / VOC) — usato dal firmware legacy via
  I²C `0x76`, ma **non presente nel CSV**. Da chiarire se è ancora nel BOM.
- Sensore di corrente motori (INA219, roadmap) — non ancora cablato.

---

## 5. Protocollo dati

### 5.1 Seriale Hub ↔ Mega (Protobuf, nanopb)

File sorgente: [infra/smartvase-proto/smartvase.proto](infra/smartvase-proto/smartvase.proto).

Messaggi atomici incapsulati in `WrapperMessage`:

- **TelemetryFast** — invio ad alta frequenza:
  `front_dist_cm`, `left_dist_cm`, `right_dist_cm`, `water_level_cm`, `lux`,
  `movement_state`, `device_id`.
  → Dopo il refactor a 6 ultrasuoni, **questo schema andrà esteso** con
  almeno `front_top_dist_cm` (US1) e `front_right_dist_cm` / `front_left_dist_cm`
  (US2/US3), oppure rinominati i campi esistenti. Decisione architetturale
  aperta.
- **TelemetryDeep** — invio a bassa frequenza (~minuti):
  BME680 (`temperature_c`, `humidity_percent`, `pressure_hpa`,
  `gas_resistance_ohms`), `uptime_s`, `free_ram_bytes`, contatori cumulativi
  (`watchdog_resets`, `total_irrigations`, `obstacles_avoided`, `stuck_events`,
  `bme_read_errors`, `log_overflows`, …), `battery_voltage`.
- **Log** — `level` (INFO/WARN/ERROR/CRITICAL), `event`, `detail`,
  `timestamp_ms`, `source_device`.
- **Heartbeat** — `uptime_s`, `is_degraded`, `device_id`.
- **Command** (Hub → Mega) — oneof: `water`, `set_mode`, `stop`,
  `request_diagnostics`, `set_motion_params`, `read_soil`, `soft_reset`.
- **CommandResponse** (Mega → Hub) — `status` (OK/ERROR), `detail`,
  `cmd_id`, `exec_time_ms`.

Generazione codice: vedere [infra/smartvase-proto/generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
Dopo la generazione: **modificare a mano** `smartvase.pb.h` cambiando
`#include <pb.h>` → `#include "pb.h"` (vincolo Arduino IDE / nanopb).

### 5.2 MQTT (JSON)

Topic root: `smartvase/{device_id}/...`. Specifica completa in
[SmartVase_data_structure.md](SmartVase_data_structure.md). Sintesi:

| Topic                                | Direzione                              |
|--------------------------------------|----------------------------------------|
| `smartvase/{id}/telemetry`           | Hub → Cloud → App                      |
| `smartvase/{id}/logs`                | Hub → Cloud (Firestore subcollection)  |
| `smartvase/{id}/alarm`               | Hub → Cloud → App (anomalie operative) |
| `smartvase/{id}/command/config`      | App → Cloud → Hub                      |
| `smartvase/{id}/command/#`           | App → Cloud → Hub (setMode, water, ecc.) |
| `smartvase/{id}/command/ack`         | Hub → Cloud → App (esito comandi: status, value, exec_time_ms) |
| `smartvase/{id}/vision/image`        | Hub/CAM → Cloud → Vision               |
| `smartvase/{id}/vision/result`       | Vision → Cloud → Hub & App             |

I JSON di `vision/result` **devono** sempre contenere `schema_version`,
`model_version`, `image_url`, `frame_quality`, `leaf_health`,
`timestamp_utc`. Enum `frame_quality`: `ok|too_dark|too_bright|blurry|occluded|unknown`.
Enum `leaf_health`: `healthy|warning|critical|unknown`.

---

## 6. Struttura del repository

```
SmartVase/
├── README.md                              # overview pubblico
├── SmartVase_data_structure.md            # spec MQTT/Firestore JSON
├── docs/
│   ├── ARCHITECTURE.md                    # questo file
│   └── PINS - Sheet1.csv                  # PIN map autoritativo
├── build_hub.bat / build_mega.bat / build_cam.bat   # wrapper PlatformIO
├── infra/
│   ├── hivemq_ca_cert.h                   # CA cert HiveMQ condiviso Hub/CAM
│   ├── smartvase-proto/                   # .proto, generatore nanopb, output
│   │   ├── smartvase.proto
│   │   ├── smartvase.pb.{c,h}
│   │   ├── generate_proto.bat
│   │   └── nanopb-nanopb-0.4.9.1/
│   └── cloud-functions/
│       └── upload-image/                  # Cloud Function stub upload JPEG
│           ├── README.md
│           ├── package.json
│           └── index.js
├── firmware/
│   ├── lib/                               # zip librerie locali (DriverDkv, HCSR04)
│   ├── 1_esp32-hub/
│   │   └── Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/
│   │       ├── platformio.ini             # esp32dev + ArduinoJson + PubSubClient + AsyncTCP
│   │       ├── include/                   # *.h + smartvase.pb.h + nanopb headers
│   │       └── src/                       # ConfigManager, WifiManager, SerialManager,
│   │                                      # MqttManager, MainLogic + nanopb runtime
│   ├── 2_platform-controller_mega/
│   │   ├── README_PlatformController_Arduino.md
│   │   └── Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/
│   │       ├── platformio.ini             # megaatmega2560 + BME680 + HCSR04 + DS3232RTC
│   │       └── src/                       # main.cpp + Movement / Sensors /
│   │                                      # Communication / Persistence / Pump /
│   │                                      # Cli / SystemStatus / Crc16 + nanopb runtime
│   └── 3_esp32-cam/
│       └── Radin_Giacomo_SmartVase_VisionCoProcessor_ESP32CAM/
│           ├── platformio.ini
│           └── src/main.cpp               # bench code — da riscrivere per Wi-Fi/MQTT
└── vision/
    ├── requirements.txt                   # opencv, numpy, pytest…
    ├── vision/
    │   ├── __init__.py
    │   ├── quality_gate.py                # brightness + Laplacian-var blur
    │   ├── metrics.py                     # HSV ratios, dominant color, bbox
    │   ├── leaf_health.py                 # classifier rule-based v0.2
    │   └── pipeline.py                    # end-to-end → JSON vision/result
    └── tests/
        ├── test_quality_gate.py
        ├── test_metrics.py
        ├── test_leaf_health.py
        └── test_pipeline.py
```

---

## 7. Architettura firmware (stato corrente)

### 7.1 Mega — `2_platform-controller_mega`

- **`main.cpp`** — setup + loop non bloccante, WDT (4s), recovery da reset
  con conteggio `watchdog_resets`, gestione `degradedMode` se `freeRam<800`
  o se Hub silente > 120 s.
- **`Movement`** — state machine `M_IDLE → M_MOVING → M_AVOID_START →
  M_AVOID_REVERSING → M_AVOID_TURNING → M_STUCK`. Light/shadow seeking
  via fotoresistore (`lightThreshold=600`). Avoidance con 3 tentativi
  prima di entrare in `M_STUCK` (cooldown auto-incrementante).
- **`Sensors`** — letture HC-SR04 con filtro EMA (α=0.4) + soglie di
  validità (2–400 cm) + streak di letture invalide consecutive. BME680
  via I²C `0x76`. Batteria via partitore. *Oggi gestisce solo 4 ultrasuoni
  e va esteso a 6.*
- **`Communication`** — framing seriale (SOF/len/payload/CRC16-CCITT),
  log queue circolare (20 slot), encode/decode `WrapperMessage`.
- **`Persistence`** — EEPROM **dual-slot** (`SLOT_0` / `SLOT_1`) con
  rotazione (wear leveling) + magic number + CRC16 per `DeviceConfig`
  e `CumulativeStats`. Write throttling (60s config, 300s stats).
- **CLI debug** su USB a 115200 baud: `status`, `stats`, `config`,
  `sensors`, `reboot`, `motor <dir> <ms>`, `pump <ms>`, `help`.

### 7.2 ESP32 Hub — `1_esp32-hub`

Architettura **FreeRTOS** con 3 task pinnati e 4 code:

| Task           | Core | Priorità | Stack | Ruolo                                       |
|----------------|------|----------|-------|---------------------------------------------|
| `TaskSerialMega` | 1  | 3 (alta) | 4 KB  | UART2 ↔ Mega, encode/decode Protobuf+framing |
| `TaskMqttLink`   | 0  | 2 (med)  | 8 KB  | Connessione TLS a HiveMQ, pub/sub           |
| `TaskMainLogic`  | 0  | 1 (bassa)| 8 KB  | Bridge JSON↔Protobuf, timer telemetria      |

Code (`xQueueCreate`):
- `serialRxQueue` (Mega → MainLogic): `SerialMessage` wrapping `WrapperMessage`.
- `serialTxQueue` (MainLogic → Mega): idem in uscita.
- `mqttTxQueue`   (MainLogic → MqttManager): `MqttMessage{topic,payload}`.
- `mqttRxQueue`   (MqttManager → MainLogic): `MqttCommand{topic,payload,ts}`.

Moduli:
- **`ConfigManager`** — NVS, struct `DeviceConfig` con WiFi/MQTT/webhook +
  magic number + CRC16.
- **`WifiManager`** — STA + fallback **Access Point di provisioning** se
  SSID vuoto o connessione fallita.
- **`SerialManager`** — Serial2 su `MEGA_RX_PIN=16 / MEGA_TX_PIN=17`.
- **`MqttManager`** — HiveMQ TLS con CA cert hardcoded, client ID da MAC,
  LWT su `smartvase/HUB_{macSuffix}/status`, sottoscrizione a
  `smartvase/HUB_{macSuffix}/command/#`.
- **`MainLogic`** — telemetry timer (60s), `checkMegaConnection` con
  deadman switch (130s, 10s di margine rispetto al Mega). Routing
  comandi MQTT → Protobuf:
  `setMode | water | stop | requestDiagnostics | setMotionParams |
   readSoil | softReset`.

> ⚠️ **Stub aperti nel Hub**: `publishTelemetryJson`, `publishLogJson`,
> `publishAlarmJson`, `processSerialMessage`, `checkMegaConnection`,
> `applyDefaultPlantLogic` sono al momento solo log placeholder.

### 7.3 ESP32-CAM — `3_esp32-cam`

Stato attuale (`main.cpp` v14): **bench code**.
Cattura JPEG → stampa header JSON + buffer raw su Serial USB → CRC32 →
stats persistenti su NVS (`successful_frames`, `failed_frames`, `crc_errors`,
rolling-avg capture time).

**Target architetturale**: Wi-Fi autonomo, pubblicazione su MQTT
(`smartvase/{id}/vision/image`) con `image_url` puntante a storage cloud.
Da progettare e implementare ex-novo.

### 7.4 Vision Python — `vision/`

- **`quality_gate.py`** — input `np.ndarray BGR` → output
  `("ok"|"too_dark"|"too_bright"|"blurry", metrics)`.
  Soglie attuali (da calibrare su immagini reali):
  `too_dark_mean_gray=40`, `too_bright_mean_gray=220`,
  `blurry_laplacian_var=60`.
- **`tests/test_quality_gate.py`** — copre i 3 casi base.
- Mancano: pipeline `leaf_health`, integrazione Firestore/MQTT,
  packaging (no `setup.py`/`pyproject.toml`).

---

## 8. Resilienza & osservabilità (concetti chiave)

- **Watchdog hardware** (Mega): WDTO_4S, conteggio reset persistito.
- **Memoria bassa**: ingresso in `degradedMode` se `freeRam < 800 B`.
- **Hub deadman timer**: Mega va in `degradedMode("Hub Missing")` se non
  riceve nulla dall'Hub per >120 s. L'Hub considera il Mega disconnesso
  dopo 130 s (margine di 10 s).
- **Persistenza resiliente**: doppio slot EEPROM + magic + CRC16, fallback
  ai default in caso di corruzione.
- **Framing seriale robusto**: SOF + len + CRC16 (poly 0x1021), state
  machine di decoding con drop dei byte malformati.
- **Log strutturati** con livelli INFO/WARN/ERROR/CRITICAL e queue circolare
  per evitare blocchi sotto carico (overflow contato in stats).

---

## 9. Workflow di sviluppo

### 9.1 Prerequisiti

- PlatformIO (consigliato CLI, già usato dai `.bat` di build).
- Python 3.x + `nanopb` (`pip install nanopb`) + `protoc`.
- Per la vision: `pip install -r vision/requirements.txt`.
- Android Studio (per l'app, fuori da questo repo).

### 9.2 Build

Dalla root del progetto:

```
build_mega.bat   # Arduino Mega
build_hub.bat    # ESP32 Hub
build_cam.bat    # ESP32-CAM
```

I `.bat` invocano `pio run -d <project>`.

### 9.3 Workflow schema-first

1. Modificare [smartvase.proto](infra/smartvase-proto/smartvase.proto).
2. Eseguire [generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
3. Copiare i `.pb.{c,h}` aggiornati in `firmware/1_esp32-hub/.../src+include`
   e in `firmware/2_platform-controller_mega/.../src`.
4. Patchare a mano `smartvase.pb.h`: `#include <pb.h>` → `#include "pb.h"`.
5. Aggiornare `smartvase_aliases.h` se sono cambiati nomi di messaggi/enum/tag.

### 9.4 Branching & autori

| Componente               | Owner principale                                   |
|--------------------------|----------------------------------------------------|
| Architettura, Hub & CAM  | Giacomo (PM & Lead Firmware)                       |
| Vision pipeline          | Antonio                                            |
| Cloud / MQTT / Firestore | Fia                                                |
| App Android              | Francesco                                          |

---

## 10. Punti aperti / TODO

Lo stato del refactor 2026-05-19 chiude la maggior parte dei TODO
architetturali del prototipo precedente. Restano i seguenti, ordinati per
priorità:

### A. Verifica/test su HW reale (priorità alta)

1. **Build e flash dei 3 firmware** con i `.bat` quando il robot è cablato:
   `build_mega.bat`, `build_hub.bat`, `build_cam.bat`. Sono stati scritti
   in ambiente sandboxato senza poter eseguire `pio run`: aspettarsi
   piccoli aggiustamenti di lib version / include path.
2. **Verifica polarità relè pompa**: in `Pump.cpp` ho usato
   `PUMP_RELAY_ACTIVE_LOW 1`. Se il modulo in uso è attivo-alto, settare
   a 0.
3. **Verifica direzione ruote**: in `Movement.cpp` "LEFT" è il canale A
   (ENA=D6, IN1=D43, IN2=D45). Da banco verificare che corrisponda alla
   ruota fisica sinistra; in caso contrario invertire IN1/IN2 o ENA/ENB.
4. **Calibrazione `light_threshold`** del fotoresistore su A1: il default
   `600` è ereditato dal vecchio prototipo, va probabilmente ritarato.
5. **Calibrazione `soil_dry_threshold`** della forcella su A0: capire il
   range ADC effettivo bagnato/asciutto e settarlo via `SetMotionParams`?
   *(in realtà va aggiunto un comando `SetSoilThreshold` o esteso `Config`).*

### B. Hardware ancora da decidere

6. **Partitore batteria**: non c'è. Quando viene aggiunto, settare in
   `Sensors.h` `BATTERY_MONITORING_ENABLED 1`, confermare `BATTERY_PIN`
   (`A2` di default) e i valori R1/R2.
7. **BME680**: mantenuto nel firmware ma non più nel CSV. Decidere se
   resta nel BOM o se va rimosso (dipende dal nuovo case).

### C. Vision pipeline

8. **Pipeline rule-based v0.2** in `vision/vision/{metrics,leaf_health,pipeline}.py`
   con 18 test pytest. Soglie HSV calibrate su foglie generiche, da
   ritarare con immagini reali del prototipo. Quando avremo dataset
   etichettato sostituire il classifier rule-based con un modello vero
   (mantenendo l'interfaccia di `classify_leaf_health`).
9. **Cloud Function `upload-image`** in `infra/cloud-functions/upload-image/`
   (Node 20 + busboy + Firebase Storage). Stub funzionale da rifinire con
   Fia: auth, App Check, rate limiting, pin CA cert lato CAM.

### D. Cloud / app

10. **App Android (Francesco)**: aggiornare il modello JSON Telemetry per
    riflettere il nuovo schema `distances_cm{top,front_right,front_left,
    left,right}` + `soil_moisture` + `water_level_cm` separato.
    Aggiungere subscription al nuovo topic `command/ack`.
11. **Seconda Cloud Function** (out-of-scope di `upload-image`): legge
    `vision/image` da HiveMQ, scarica il JPEG via `image_url`, invoca la
    pipeline Python `vision.analyze_image`, pubblica `vision/result`.

### E. Pulizia / housekeeping

12. **CLI debug Mega**: implementata in `Cli.{h,cpp}`. Comandi: `help`,
    `status`, `stats`, `config`, `sensors`, `mode`, `motor`, `pump`,
    `reboot`. Disponibile sulla USB Serial del Mega.

---

## 11. Convenzioni per chi (umano o AI) lavora qui

- **Lingua**: italiano per commit, doc e conversazione. Identificatori e
  log in inglese.
- **Sorgenti `OLD_*`**: trattarli come storico / referenza, **mai** come
  base di edit attivi.
- **PIN**: il CSV `docs/PINS - Sheet1.csv` è la **single source of truth**
  del cablaggio. Qualsiasi `#define`/`const int` nel codice che lo
  contraddice è un bug.
- **Protobuf**: non modificare a mano i `.pb.{c,h}` (a parte la patch di
  include obbligatoria). Le modifiche partono sempre da `.proto`.
- **Allocazioni**: niente `String` su Mega (SRAM limitata). Buffer fissi
  con `strncpy` + null-termination esplicita.
- **Non-blocking**: nessun `delay()` nel main loop Mega. Solo `millis()`
  + state machines.
- **EEPROM**: rispettare il write throttling (`EEPROM_*_WRITE_INTERVAL`)
  per non logorare le celle.
