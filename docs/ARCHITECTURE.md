# SmartVase — Architecture and project context

> Technical onboarding document for the SmartVase team.
> State consolidated as of **2026-07-02**. Aligned with the PIN map
> `docs/PINS - Sheet1.csv` and the current architectural decisions.
> The behavioral/product design of the autonomous care layer lives in
> `docs/Plant_Care_Design.md`.

---

## 1. Current project status (TL;DR)

- The hardware prototype is in **lab bring-up** (June–July 2026): motors and
  sensors partially verified on the bench, the PIN map (`docs/PINS - Sheet1.csv`)
  is the authoritative wiring reference.
- Firmware versions (all build offline):
  - Mega: **v5.3** — 6 HC-SR04, pump via relay with tank protection, RTC
    DS3232 (+ software fallback clock), UVA grow lights, VNH5019 motor driver,
    proportional steering + wall-following, and the **autonomous plant-care
    layer** (`Care` + `CarePolicy.h`: daily light budget, rotating light scan,
    dose/soak/verify watering, plant profiles — see
    `docs/Plant_Care_Design.md`). SRAM hysteresis (800/1200 B) for a clean
    recovery from degraded mode.
  - Hub: **v1.3** — publishes telemetry, logs, alarms and **command/ack**;
    deadman switch active; TLS to HiveMQ with NTP kick; non-blocking MQTT
    reconnect; OTA (to be bench-validated); AP provisioning + captive portal.
  - ESP32-CAM: **v2.2** — Wi-Fi STA + NTP + MQTT TLS + **streaming** HTTP
    upload to a Cloud Function, plus on-device C++ leaf-health analysis (`VisionBotanist.cpp`).
  - Protocol: **proto v4.1** — `TelemetryFast` with 5 nav distances
    + soil moisture + epoch_s; `CommandResponse` with a `value` field;
    `TelemetryDeep` extended with the autonomous-care daily KPIs
    (state, light-budget %, relocations, doses, UVA minutes — tags 22-27,
    published by the Hub as the `care` JSON object).
- **Vision pipeline**: on-device leaf-health analysis (HSV metrics, quality gate,
  foliage coverage, green/brown ratios) is implemented in C++ on the ESP32-CAM
  (`VisionBotanist.cpp`). The Python scripts in `vision/` are used offline for
  prototyping, testing, and calibrating the edge algorithm.
- **Cloud Function stub** `upload-image` added in `infra/cloud-functions/`
  (Node 20 + Firebase Storage). To be refined with Fia.

---

## 2. Product vision

SmartVase is a **mobile, autonomous IoT pot/greenhouse**:

- It moves on wheels seeking light or shade, either on command
  (`LIGHT` / `SHADOW` / `IDLE` modes) or **autonomously**: with the care layer
  enabled (Mega v5.3, `care on`) the robot manages the plant's day on its own,
  keeping a daily **light budget** and the soil moisture inside the range of
  the selected plant profile (design: `docs/Plant_Care_Design.md`).
- It waters the plant on command (relay-controlled pump) or autonomously in
  **dose/soak/verify** cycles based on soil moisture (fork sensor), always
  under the tank-protection and duration-cap safeties.
- It periodically captures images of the plant with the ESP32-CAM and evaluates
  frame quality and leaf health via the Python vision pipeline.
- It is controlled by an **Android app** (MVVM + Compose, developed by Francesco).
- It reports telemetry and logs to HiveMQ Cloud, with Firestore as the authoritative store.

Project guiding principles (from the README):
**Resilience & Robustness** · **Observability & Diagnostics** ·
**Performance & Efficiency** · **Modularity & Maintainability**.

---

## 3. Hardware topology

Three microcontrollers plus a mobile app:

| Role                 | MCU             | Codename     | Task                                                                    |
|----------------------|-----------------|--------------|-------------------------------------------------------------------------|
| Platform Controller  | Arduino Mega    | *The Brawn*  | Direct control of motors, pump, sensors, RTC. No networking.            |
| Logic & Web Hub      | ESP32 standard  | *The Brain*  | Wi-Fi, MQTT/TLS to HiveMQ, coordination, JSON↔Protobuf bridge.          |
| Vision Co-Processor  | ESP32-CAM       | *The Eye*    | JPEG capture, on-edge C++ leaf-health analysis, image upload, MQTT.     |
| Android App          | —               | —            | User UI (under local development, tracked separately).                  |

### 3.1 Communication buses

- **Hub ↔ Mega**: serial UART (115200 baud). Frame:
  `SOF=0xAA | len_hi | len_lo | payload(protobuf) | crc16_hi | crc16_lo`.
  Payload = `WrapperMessage` from [smartvase.proto](infra/smartvase-proto/smartvase.proto).
- **Hub ↔ Cloud**: MQTT over TLS to **HiveMQ Cloud**. **JSON** payload.
- **Cloud pipeline**:
  `HiveMQ ⇄ Cloud Functions ⇄ Firestore ⇄ Android App / Vision`.
  Firestore is the authoritative store. The Cloud Functions bridge MQTT and
  Firestore documents (see [SmartVase_data_structure.md](SmartVase_data_structure.md)).
- **CAM ↔ Cloud**: **autonomous**. The ESP32-CAM connects to Wi-Fi, captures images,
  performs on-edge C++ leaf-health analysis (`VisionBotanist.cpp`), uploads images to
  storage, and publishes results directly to MQTT.
- **Python Vision**: used offline in `vision/` for prototyping and calibrating
  the C++ edge algorithm.

---

## 4. Authoritative PIN map (Arduino Mega)

From the file [docs/PINS - Sheet1.csv](docs/PINS - Sheet1.csv).
**This table wins over any `#define` present in the current firmware
sources.** All Mega firmware must be aligned to these pins.

### 4.1 HC-SR04 ultrasonic sensors (6 total)

| ID  | Physical role                      | Trigger | Echo | Notes                                  |
|-----|------------------------------------|---------|------|----------------------------------------|
| US1 | Front-top (forward, high)          | D33     | D35  | Upper front anti-collision             |
| US2 | Front-right                        | D26     | D27  | Front-right anti-collision             |
| US3 | Front-left                         | D36     | D37  | Front-left anti-collision              |
| US4 | Water tank                         | D50     | D51  | **Measures the water level in the tank** |
| US5 | Left side                          | D4      | D5   | Left-side anti-collision               |
| US6 | Right side                         | D28     | D29  | Right-side anti-collision              |

US1, US2, US3, US5, US6 → **navigation and obstacle avoidance** (5 sensors).
US4 → **only** the water level in the tank.

### 4.2 Motor driver — Pololu Dual VNH5019 Shield (2 DC motors)

The motor shield is a **Pololu Dual VNH5019** (`ash02b`, 2014), wired to the Mega
with jumpers. VNH5019 interface per motor (≠ L298N): `INA`/`INB` (direction),
`PWM` (speed, PWM pin), `EN/DIAG` (enable + fault flag, open-drain with a
pull-up on the shield → driver enabled at rest).

| Shield signal  | Mega pin | Notes |
|----------------|----------|-------|
| M1PWM          | D7 (PWM) | "left" motor speed |
| M1INA          | D41      | direction |
| M1INB          | D43      | direction |
| M1EN/DIAG      | not wired | optional, not connected as of 2026-06-30 |
| M2PWM          | D6 (PWM) | "right" motor speed |
| M2INA          | D45      | direction |
| M2INB          | D47      | direction |
| M2EN/DIAG      | not wired | optional, not connected as of 2026-06-30 |

> Pin mapping **confirmed 2026-06-30** via multimeter continuity test (see
> `docs/Hardware_Verification_Sheet.md` §1.1/T7), superseding an earlier
> best-guess mapping that paired PWM/INA/INB from different shield channels —
> the actual root cause of the 0 V outputs seen on the bench.
>
> [Movement.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Movement.cpp)
> drives PWM (`analogWrite`) + INA/INB (`digitalWrite`). EN/DIAG is not wired to
> the Mega (`MOTOR_EN_DIAG_WIRED 0`): `faultLeft()`/`faultRight()` always
> return `false` until it is physically wired and the flag/pins are updated.
> Which motor is "left" and the wheel direction must still be verified on the
> bench (`motortest`).
> ⚠️ **Common GND** Mega↔shield is mandatory: without a ground reference the
> signals do not arrive and the outputs stay at 0 V even with VDD present.

### 4.3 Power and RTC

| Function                       | Mega pin       | Notes                                                  |
|--------------------------------|----------------|-------------------------------------------------------|
| Battery voltage divider        | A2 (`BATTERY_MONITORING_ENABLED=0`) | Divider R1=30k, R2=7.5k → `Vbatt = Vadc · 5.0`; not mounted |
| RTC DS3232 — SDA               | D20 (SDA)      | I²C 0x68                                              |
| RTC DS3232 — SCL               | D21 (SCL)      | I²C 0x68                                              |

> **RTC — software fallback clock + Hub NTP sync**: if the DS3232 chip does not
> respond on I²C or has a stopped oscillator, the Mega runs a `millis()`-based
> software clock; at boot, if no valid time is available, it starts from
> **08:00** (`DEFAULT_BOOT_HOUR`, inside the grow-light daylight window). Since
> proto v4.2 the software clock is **re-synced by every Hub heartbeat** carrying
> the Hub's NTP epoch (~30 s period, see §5.1), so with a connected Hub the Mega
> tracks real time with negligible drift and no manual `rtc set`. Standalone
> bench sessions still use `rtc set <epoch>`. While the software clock is
> active it takes precedence over the chip (a flaky chip with an unset time
> must not hijack a good clock); a healthy chip is opportunistically kept
> aligned by the Hub syncs. ⚠️ Bench status 2026-07-06: the HW-084 module
> (DS3231) is **faulty** — the I²C bus is healthy (BME680 at 0x76 answers,
> `i2cscan` CLI) but both chips on the RTC module are silent; the Hub sync IS
> the time source until the module is replaced.

### 4.4 Pump, relays and grow lights

| Function               | Mega pin | Notes                                |
|------------------------|----------|-------------------------------------|
| Relay channel 1 (IN1)  | D10      | Irrigation pump (active LOW, `Pump` module) |
| Relay channel 2 (IN2)  | D11      | **UVA grow lights** (`GrowLight` module) |

- **Pump** ([Pump.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Pump.cpp)):
  relay D10 active-LOW, 60 s duration cap, empty-tank protection (US4). The
  `WaterCommand` is implemented end-to-end.
- **UVA lights** ([GrowLight.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/GrowLight.cpp)):
  relay D11, lights wired on the **NC** contact (with the relay at rest they are
  ON, polarity inverted compared to the pump). Two driving policies:
  - **care layer active** (v5.3): the lights are the **end-of-day budget
    top-up** — on only when, near the close of the daylight window, the daily
    light budget is still in deficit, with a per-day cap (`CARE_TOP_UP` state,
    see `docs/Plant_Care_Design.md` §6);
  - **care layer off** (legacy rule): on only if `IDLE` mode **and**
    `lux < light_threshold` **and** within the daylight window **06:00–20:00**
    (gated via RTC/software clock; outside that window or without a valid time
    they stay off). Pure logic in
    [`growLightWanted()` / `withinDaylightWindow()`](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/SensorPolicy.h).

### 4.5 "Fork" soil-moisture sensor

| Function           | Pin       | Notes                                               |
|--------------------|-----------|-----------------------------------------------------|
| Fork (signal)      | A0        | Two-prong probe for soil moisture                   |
| Fork VCC           | 5V        |                                                     |
| Fork GND           | GND       |                                                     |

> **Photoresistor ↔ fork conflict on A0**: the CSV maps both to A0, but on the
> Mega each ADC is single-ended. Decision: **the fork stays on A0, the
> photoresistor moves to another free analog pin** (e.g. A1 or A3). To be
> decided and noted at refactor time.

### 4.6 Photoresistor (ambient light)

| Function           | Pin       | Notes                                               |
|--------------------|-----------|-----------------------------------------------------|
| Photoresistor (LDR)| **TBD**   | Was on A0 in the legacy firmware. To be moved to make room for the fork. |

The photoresistor drives the `LIGHT` / `SHADOW` state machine:
turn right if more light is needed, left if shade is needed.

### 4.7 Mega pins currently *unassigned* / *unclear*

- BME680 sensor (T / RH / pressure / VOC) — used by the legacy firmware via
  I²C `0x76`, but **not present in the CSV**. To be clarified whether it is still
  in the BOM.
- Motor current sensor (INA219, roadmap) — not yet wired.

---

## 5. Data protocol

### 5.1 Hub ↔ Mega serial (Protobuf, nanopb)

Source file: [infra/smartvase-proto/smartvase.proto](infra/smartvase-proto/smartvase.proto).

Atomic messages wrapped in `WrapperMessage`:

- **TelemetryFast** — high-frequency send:
  `front_dist_cm`, `left_dist_cm`, `right_dist_cm`, `water_level_cm`, `lux`,
  `movement_state`, `device_id`.
  → After the refactor to 6 ultrasonic sensors, **this schema must be extended**
  with at least `front_top_dist_cm` (US1) and `front_right_dist_cm` /
  `front_left_dist_cm` (US2/US3), or the existing fields must be renamed. Open
  architectural decision.
- **TelemetryDeep** — low-frequency send (~minutes):
  BME680 (`temperature_c`, `humidity_percent`, `pressure_hpa`,
  `gas_resistance_ohms`), `uptime_s`, `free_ram_bytes`, cumulative counters
  (`watchdog_resets`, `total_irrigations`, `obstacles_avoided`, `stuck_events`,
  `bme_read_errors`, `log_overflows`, …), `battery_voltage`.
- **Log** — `level` (INFO/WARN/ERROR/CRITICAL), `event`, `detail`,
  `timestamp_ms`, `source_device`.
- **Heartbeat** — `uptime_s`, `is_degraded`, `device_id`, `epoch_s`
  (proto v4.2). In the Hub→Mega direction the heartbeat doubles as the
  Mega's **time source**: the Hub attaches its NTP epoch (UTC; `0` = "no
  NTP time yet", ignored) and the Mega re-bases its software clock on it —
  the hardware RTC module proved faulty on the bench (2026-07-06: I²C bus
  healthy, BME680 answers, DS3231 and its on-module EEPROM silent). The
  software clock stays authoritative between syncs (worst-case drift = one
  30 s heartbeat period) and a half-dead RTC chip that occasionally ACKs
  cannot hijack the time (see `Sensors::syncEpochFromHub`,
  `hubEpochPlausible` in `CommandPolicy.h`). Timezone offset applied on the
  Mega (`HUB_EPOCH_TZ_OFFSET_S`, Sensors.cpp).
- **Command** (Hub → Mega) — oneof: `water`, `set_mode`, `stop`,
  `request_diagnostics`, `set_motion_params`, `read_soil`, `soft_reset`.
- **CommandResponse** (Mega → Hub) — `status` (OK/ERROR), `detail`,
  `cmd_id`, `exec_time_ms`.

Code generation: see [infra/smartvase-proto/generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
After generation: **manually edit** `smartvase.pb.h` changing
`#include <pb.h>` → `#include "pb.h"` (Arduino IDE / nanopb constraint).

### 5.2 MQTT (JSON)

Root topic: `smartvase/{device_id}/...`. Full spec in
[SmartVase_data_structure.md](SmartVase_data_structure.md). Summary:

| Topic                                | Direction                              |
|--------------------------------------|----------------------------------------|
| `smartvase/{id}/telemetry`           | Hub → Cloud → App                      |
| `smartvase/{id}/logs`                | Hub → Cloud (Firestore subcollection)  |
| `smartvase/{id}/alarm`               | Hub → Cloud → App (operational anomalies) |
| `smartvase/{id}/command/config`      | App → Cloud → Hub                      |
| `smartvase/{id}/command/#`           | App → Cloud → Hub (setMode, water, etc.) |
| `smartvase/{id}/command/ack`         | Hub → Cloud → App (command result: status, value, exec_time_ms) |
| `smartvase/{id}/vision/image`        | Hub/CAM → Cloud → Vision               |
| `smartvase/{id}/vision/result`       | Vision → Cloud → Hub & App             |

The `vision/result` JSON **must** always contain `schema_version`,
`model_version`, `image_url`, `frame_quality`, `leaf_health`,
`timestamp_utc`. Enum `frame_quality`: `ok|too_dark|too_bright|blurry|occluded|unknown`.
Enum `leaf_health`: `healthy|warning|critical|unknown`.

---

## 6. Repository structure

```
SmartVase/
├── README.md                              # public overview
├── SmartVase_data_structure.md            # MQTT/Firestore JSON spec
├── docs/
│   ├── ARCHITECTURE.md                    # this file
│   └── PINS - Sheet1.csv                  # authoritative PIN map
├── build_hub.bat / build_mega.bat / build_cam.bat   # PlatformIO wrappers
├── infra/
│   ├── hivemq_ca_cert.h                   # HiveMQ CA cert shared by Hub/CAM
│   ├── smartvase-proto/                   # .proto, nanopb generator, output
│   │   ├── smartvase.proto
│   │   ├── smartvase.pb.{c,h}
│   │   ├── generate_proto.bat
│   │   └── nanopb-nanopb-0.4.9.1/
│   └── cloud-functions/
│       └── upload-image/                  # JPEG upload Cloud Function stub
│           ├── README.md
│           ├── package.json
│           └── index.js
├── firmware/
│   ├── lib/                               # local library zips (DriverDkv, HCSR04)
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
│           └── src/main.cpp               # bench code — to be rewritten for Wi-Fi/MQTT
└── vision/
    ├── requirements.txt                   # opencv, numpy, pytest…
    ├── vision/
    │   ├── __init__.py
    │   ├── quality_gate.py                # brightness + Laplacian-var blur
    │   ├── metrics.py                     # HSV ratios, dominant color, bbox
    │   ├── leaf_health.py                 # rule-based classifier v0.2
    │   └── pipeline.py                    # end-to-end → JSON vision/result
    └── tests/
        ├── test_quality_gate.py
        ├── test_metrics.py
        ├── test_leaf_health.py
        └── test_pipeline.py
```

---

## 7. Firmware architecture (current state)

### 7.1 Mega — `2_platform-controller_mega`

- **`main.cpp`** — setup + non-blocking loop, WDT (4s), reset recovery
  with `watchdog_resets` counting, `degradedMode` handling if `freeRam<800`
  or if the Hub is silent for > 120 s.
- **`Movement`** — state machine `M_IDLE → M_MOVING → M_AVOID_START →
  M_AVOID_REVERSING → M_AVOID_TURNING → M_STUCK` (+ the internal
  `M_SCAN_ROTATE / M_SCAN_ALIGN` light-scan states, reported as `MOVING` in
  telemetry). In `M_MOVING` it uses **proportional differential steering**
  (`NavPolicy.h`, see below) instead of the old bang-bang stop-and-turn:
  continuous steering away from the closer side, speed ramped down in the slow
  zone, with the hard `< emergencyCm` case still handled by the reverse/turn
  recovery FSM; a target mode back to `IDLE` now stops immediately (no longer
  waits for the 20 s safety timeout). Light/shadow seeking (`light_threshold`,
  default **500**, tunable via `light <adc>`) becomes a gentle steering bias.
  **Rotating light scan** (`startLightScan`, v5.3): with a single fixed LDR the
  light direction is obtained by rotating in place for ~one turn while
  accumulating the mean ADC over 12 time sectors, then rotating again to the
  best sector (brightest/darkest) and resuming the gradient climb — the "solar
  compass" used by the care layer on every relocation. Optional
  **wall-following** sub-mode (`wall <left|right|off>`, bench-only) via the
  side sensors. All maneuvers route through a signed `driveMotors(left,right)`
  primitive. Driver: **Pololu Dual VNH5019** (see §4.2), PWM/INA/INB
  (+ EN/DIAG fault read when wired).
- **`NavPolicy.h`** — pure, host-testable navigation logic (no HW):
  `proportionalDrive` (zone-based differential command + emergency flag),
  `wallFollowDrive` (side-sensor P controller), NaN-safe. Unit-tested in
  `tests/host/test_nav_policy.cpp`.
- **`Care` / `CarePolicy.h`** (v5.3) — the **autonomous plant-care layer**
  (homeostasis, L2 in the layered model of `docs/Plant_Care_Design.md` §2).
  `CarePolicy.h` is pure and host-testable (`tests/host/test_care_policy.cpp`):
  plant profiles with shade/medium/sun presets, daily **light budget**
  (relative DLI proxy — LDR ADC normalized against the auto-calibrated room
  maximum and integrated over time), scan sector selection, dose/soak/verify
  watering decision, and the `careStep()` daily state machine
  (`NIGHT → SEEK_SUN → BASK → SEEK_SHADE/SHELTER → TOP_UP`). `Care.cpp` runs
  it at 1 Hz and applies it to the actuators, adding: daily KPI counters
  (budget %, doses, relocations, UVA minutes — `care` CLI + `care_day_end`
  log), a **manual-override suspension** (a CLI/Hub `setMode` pauses autonomy
  for 30 min), and full deference to the L0 safeties (degraded mode, tank
  guard, caps). Profile and enable flag persisted in `DeviceConfig`
  (EEPROM); **disabled by default**, enabled with `care on`.
- **`Sensors`** — readings of the **6** round-robin HC-SR04 with a **median-of-3
  anti-bounce pre-filter** (`medianOf3`) feeding an EMA filter (α=0.4) +
  validity thresholds (2–200/120 cm) + invalid-reading streaks. RTC DS3232 with
  a **software fallback clock** (see §4.3). BME680 and battery behind flags
  (not mounted).
- **`Pump`** — relay D10 (active-LOW), 60 s cap, empty-tank protection.
- **`GrowLight`** — relay D11, UVA lights on the NC contact; with the care
  layer active they are the end-of-day budget top-up, otherwise the legacy
  rule applies: on in IDLE + insufficient light + daylight window
  06:00–20:00 (see §4.4).
- **`Communication`** — serial framing (SOF/len/payload/CRC16-CCITT),
  circular log queue (20 slots), encode/decode `WrapperMessage`, `cmd_id`
  idempotency + command clamp/rate-limit (`CommandPolicy.h`).
- **`Persistence`** — **dual-slot** EEPROM (`SLOT_0` / `SLOT_1`) with
  rotation (wear leveling) + magic number + CRC16 for `DeviceConfig`
  and `CumulativeStats`. Write throttling (60s config, 300s stats).
- **Debug CLI** over USB at 115200 baud: `status`, `stats`, `config`,
  `sensors`, `diag`, `reboot`, `mode`, `plant [shade|medium|sun]`,
  `care [on|off]`, `wall <left|right|off>`, `motor <dir> <ms>`
  (max 60 s), `motortest`, `pump <ms>`, `tank <cm>`, `light <adc>`, `calib`,
  `rtc`/`rtc set`, `standalone`, `help`.

### 7.2 ESP32 Hub — `1_esp32-hub`

**FreeRTOS** architecture with 3 pinned tasks and 4 queues:

| Task           | Core | Priority | Stack | Role                                        |
|----------------|------|----------|-------|---------------------------------------------|
| `TaskSerialMega` | 1  | 3 (high) | 4 KB  | UART2 ↔ Mega, Protobuf+framing encode/decode |
| `TaskMqttLink`   | 0  | 2 (med)  | 8 KB  | TLS connection to HiveMQ, pub/sub           |
| `TaskMainLogic`  | 0  | 1 (low)  | 8 KB  | JSON↔Protobuf bridge, telemetry timer       |

Queues (`xQueueCreate`):
- `serialRxQueue` (Mega → MainLogic): `SerialMessage` wrapping `WrapperMessage`.
- `serialTxQueue` (MainLogic → Mega): same in the outbound direction.
- `mqttTxQueue`   (MainLogic → MqttManager): `MqttMessage{topic,payload}`.
- `mqttRxQueue`   (MqttManager → MainLogic): `MqttCommand{topic,payload,ts}`.

Modules:
- **`ConfigManager`** — NVS, `DeviceConfig` struct with WiFi/MQTT/webhook +
  magic number + CRC16.
- **`WifiManager`** — STA + **provisioning Access Point** fallback if the
  SSID is empty or the connection fails.
- **`SerialManager`** — Serial2 on `MEGA_RX_PIN=16 / MEGA_TX_PIN=17`.
- **`MqttManager`** — HiveMQ TLS with hardcoded CA cert, client ID from the MAC,
  LWT on `smartvase/HUB_{macSuffix}/status`, subscription to
  `smartvase/HUB_{macSuffix}/command/#`.
- **`MainLogic`** — telemetry timer (60s), `checkMegaConnection` with
  deadman switch (130s, a 10s margin over the Mega). MQTT command → Protobuf
  routing:
  `setMode | water | stop | requestDiagnostics | setMotionParams |
   readSoil | softReset`.

> ⚠️ **Open stubs in the Hub**: `publishTelemetryJson`, `publishLogJson`,
> `publishAlarmJson`, `processSerialMessage`, `checkMegaConnection`,
> `applyDefaultPlantLogic` are currently only log placeholders.

### 7.3 ESP32-CAM — `3_esp32-cam`

Current state (`main.cpp` v14): **bench code**.
JPEG capture → prints JSON header + raw buffer over USB Serial → CRC32 →
stats persisted to NVS (`successful_frames`, `failed_frames`, `crc_errors`,
rolling-avg capture time).

**Architectural target**: autonomous Wi-Fi, publishing over MQTT
(`smartvase/{id}/vision/image`) with `image_url` pointing to cloud storage.
To be designed and implemented from scratch.

### 7.4 Vision Pipeline — C++ Edge & Python Prototyping

- **On-Edge C++ Analysis (`firmware/3_esp32-cam/.../src/VisionBotanist.cpp`)** — executes
  leaf-health analysis directly on the ESP32-CAM (RGB to HSV decoding, circular ROI
  filtering, green/brown ratio calculation, and foliage quality assessment).
- **Python Prototyping (`vision/`)** — contains `pixel_analyzer.py` and `tests/test_analyzer.py`,
  used offline for testing, prototyping, and calibrating the C++ edge algorithm.

---

## 8. Resilience & observability (key concepts)

- **Hardware watchdog** (Mega): WDTO_4S, persisted reset count.
- **Low memory**: enters `degradedMode` if `freeRam < 800 B`.
- **Hub deadman timer**: the Mega enters `degradedMode("Hub Missing")` if it
  receives nothing from the Hub for >120 s. The Hub considers the Mega
  disconnected after 130 s (10 s margin).
- **Resilient persistence**: dual EEPROM slot + magic + CRC16, fallback to
  defaults on corruption.
- **Robust serial framing**: SOF + len + CRC16 (poly 0x1021), decoding state
  machine that drops malformed bytes.
- **Structured logs** with INFO/WARN/ERROR/CRITICAL levels and a circular queue
  to avoid blocking under load (overflow counted in stats).

---

## 9. Development workflow

### 9.1 Prerequisites

- PlatformIO (CLI recommended, already used by the build `.bat` files).
- Python 3.x + `nanopb` (`pip install nanopb`) + `protoc`.
- For vision: `pip install -r vision/requirements.txt`.
- Android Studio (for the app, outside this repo).

### 9.2 Build

From the project root:

```
build_mega.bat   # Arduino Mega
build_hub.bat    # ESP32 Hub
build_cam.bat    # ESP32-CAM
```

The `.bat` files invoke `pio run -d <project>`.

### 9.3 Schema-first workflow

1. Edit [smartvase.proto](infra/smartvase-proto/smartvase.proto).
2. Run [generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
3. Copy the updated `.pb.{c,h}` into `firmware/1_esp32-hub/.../src+include`
   and `firmware/2_platform-controller_mega/.../src`.
4. Manually patch `smartvase.pb.h`: `#include <pb.h>` → `#include "pb.h"`.
5. Update `smartvase_aliases.h` if message/enum/tag names changed.

### 9.4 Branching & authors

| Component                | Main owner                                         |
|--------------------------|----------------------------------------------------|
| Architecture, Hub & CAM  | Giacomo (PM & Lead Firmware)                       |
| Vision pipeline          | Antonio (Edge C++ algorithm & Python calibration)  |
| Cloud / MQTT / Firestore | Fia                                                |
| Android App              | Francesco (under local development)                |

---

## 10. Open items / TODO

The 2026-05-19 refactor closes most of the architectural TODOs of the previous
prototype. The following remain, ordered by priority:

### A. Verification/testing on real HW (high priority)

1. **Build and flash the 3 firmwares** with the `.bat` files once the robot is
   wired: `build_mega.bat`, `build_hub.bat`, `build_cam.bat`. They were written
   in a sandboxed environment without being able to run `pio run`: expect small
   lib-version / include-path adjustments.
2. **Verify the pump relay polarity**: in `Pump.cpp` I used
   `PUMP_RELAY_ACTIVE_LOW 1`. If the module in use is active-high, set it to 0.
2-bis. **VNH5019 motor driver**: PWM/INA/INB pin mapping confirmed via
   continuity test on 2026-06-30 (see §4.2); still to verify on the bench: the
   **common GND** Mega↔shield, and whether the motors actually spin now that
   the mapping is fixed. EN/DIAG is not wired, so `diag`'s `fault L/R` is
   currently a placeholder (`n/d`) — wire it and set `MOTOR_EN_DIAG_WIRED 1`
   in `Movement.cpp` if fault diagnostics are wanted.
3. **Verify the wheel direction**: in `Movement.cpp` "LEFT" is motor 1
   (PWM=D7, INA=D41, INB=D43). On the bench, verify it matches the physical
   left wheel; otherwise swap INA/INB. Use `motortest`.
4. **Calibrate `light_threshold`** of the photoresistor on A1: new default
   **500** (tuned 2026-06-30: dark≈11, lab neon≈540, real scale ~0-800).
   Tunable on the bench with `light <adc>`. Used both by the seeking and the UVA lights.
5. **Calibrate `soil_dry_threshold`** of the fork on A0: figure out the actual
   wet/dry ADC range and set it via `SetMotionParams`?
   *(in reality a `SetSoilThreshold` command should be added, or `Config` extended).*

### B. Hardware still to be decided

6. **Battery divider**: not present. When added, set
   `BATTERY_MONITORING_ENABLED 1` in `Sensors.h`, confirm `BATTERY_PIN`
   (`A2` by default) and the R1/R2 values.
7. **BME680**: kept in the firmware but no longer in the CSV. Decide whether it
   stays in the BOM or is removed (depends on the new case).

### C. Vision pipeline

8. **On-Edge C++ Vision Algorithm** in `firmware/3_esp32-cam/.../src/VisionBotanist.cpp`.
   HSV thresholds and circular ROI analysis implemented on the ESP32-CAM. Python scripts
   in `vision/` are maintained for offline testing and calibration of these parameters.
9. **`upload-image` Cloud Function** in `infra/cloud-functions/upload-image/`
   (Node 20 + busboy + Firebase Storage). Functional stub to be refined with
   Fia: auth, App Check, rate limiting, CA cert pinning on the CAM side.

### D. Cloud / app

10. **Android app (Francesco)**: update the Telemetry JSON model to reflect the
    new schema `distances_cm{top,front_right,front_left,left,right}` +
    `soil_moisture` + separate `water_level_cm`.
    Add a subscription to the new `command/ack` topic.
11. **Cloud Vision Processing**: with leaf-health analysis now running on-edge via
    `VisionBotanist.cpp` on the ESP32-CAM, cloud-side image evaluation is optional
    and superseded by edge results published directly to MQTT.

### E. Cleanup / housekeeping

12. **Mega debug CLI**: implemented in `Cli.{h,cpp}`. Commands: `help`,
    `status`, `stats`, `config`, `sensors`, `mode`, `motor`, `pump`,
    `reboot`. Available on the Mega's USB Serial.

---

## 11. Conventions for whoever (human or AI) works here

- **Language**: project documentation in **English** (the course is delivered in
  English). Identifiers and logs in English. Conversation can be in Italian.
- **`OLD_*` sources**: treat them as history / reference, **never** as a base for
  active edits.
- **PINs**: the CSV `docs/PINS - Sheet1.csv` is the **single source of truth**
  for wiring. Any `#define`/`const int` in the code that contradicts it is a bug.
- **Protobuf**: do not manually edit the `.pb.{c,h}` (except the mandatory
  include patch). Changes always start from `.proto`.
- **Allocations**: no `String` on the Mega (limited SRAM). Fixed buffers with
  `strncpy` + explicit null-termination.
- **Non-blocking**: no `delay()` in the Mega main loop. Only `millis()`
  + state machines.
- **EEPROM**: respect the write throttling (`EEPROM_*_WRITE_INTERVAL`) to avoid
  wearing out the cells.
