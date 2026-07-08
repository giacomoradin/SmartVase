<p align="center">
  <img src="https://raw.githubusercontent.com/giacomoradin/SmartVase/main/docs/assets/logo_SmartVase.png" alt="SmartVase logo" width="140">
</p>

<h1 align="center">SmartVase — IoT Mobile Greenhouse</h1>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  <img src="https://img.shields.io/badge/platform-AVR%20%7C%20ESP32-blue" alt="Platforms">
  <img src="https://img.shields.io/badge/build-PlatformIO-orange" alt="PlatformIO">
</p>

<p align="center">
  <sub>University of Trento — Embedded Software for the Internet of Things</sub><br>
  <img src="https://raw.githubusercontent.com/giacomoradin/SmartVase/main/docs/assets/logo_unitn.png" alt="University of Trento" width="48">
</p>

SmartVase is a mobile, self-watering planter. A wheeled robot seeks light or
shade depending on the plant's needs, waters the plant from an on-board tank,
supplements light with UVA grow lights, periodically photographs the foliage
for leaf-health analysis, and reports telemetry, logs and alarms over MQTT.
An autonomous care layer can manage the plant's whole day: daily light
budget, relocation, dose/soak/verify watering cycles and an end-of-day UVA
top-up, driven by per-plant profiles.

## Table of Contents
1. [Idea of the Project](#1-idea-of-the-project)
2. [Requirements](#2-requirements)
3. [Project Layout](#3-project-layout)
4. [How to Build, Burn and Run](#4-how-to-build-burn-and-run)
5. [User Guide](#5-user-guide)
6. [Software Architecture](#6-software-architecture)
7. [Testing](#7-testing)
8. [Documentation](#8-documentation)
9. [Project Status](#9-project-status)
10. [Presentation and Demo Video](#10-presentation-and-demo-video)
11. [Team Members and Contributions](#11-team-members-and-contributions)
12. [License](#12-license)

## 1. Idea of the Project

**Problem.** House plants need the right amount of light and water, but a pot
cannot move toward the light and its owner is not always home. Wrong exposure
and forgotten (or excessive) watering are the most common causes of failure.

**Solution.** A robotic planter that measures its environment and acts on it:
it navigates toward light or shade while avoiding obstacles, irrigates with
bounded, verified doses, and reports everything to the cloud so the user can
monitor and control it remotely.

**Working scheme.** The system is split across three microcontrollers with
strict separation of concerns, plus a cloud backbone and a mobile app:

| Component | Codename | Role |
|---|---|---|
| Arduino Mega 2560 | *The Brawn* | Real-time platform control: 6 ultrasonic sensors, motor driver, pump and grow-light relays, RTC, safety layer. No network access. |
| ESP32 DevKit | *The Brain* | Gateway: Wi-Fi, MQTT over TLS, NTP, JSON/Protobuf bridge on FreeRTOS (3 pinned tasks + queues) |
| ESP32-CAM (OV2640) | *The Eye* | Periodic JPEG capture, on-edge leaf-health analysis, cloud upload, MQTT publish |

```
Mega  <== UART 115200 (Protobuf/nanopb, CRC16 framing) ==>  ESP32 Hub
ESP32 Hub / ESP32-CAM  <== MQTT over TLS ==>  HiveMQ Cloud  ==>  Firestore  ==>  Mobile app
```

The user interacts through the mobile companion app (separate repository) or,
on the bench, through the serial CLIs available on all three boards.

## 2. Requirements

### Hardware

| Qty | Component | Purpose |
|---|---|---|
| 1 | Arduino Mega 2560 | Platform controller (86 GPIO, 16 ADC channels, 4 UART) |
| 1 | ESP32 DevKit (esp32dev) | Logic and web hub |
| 1 | ESP32-CAM AI-Thinker + OV2640 (PSRAM) | Vision co-processor |
| 6 | HC-SR04 ultrasonic sensor | 5 for navigation, 1 for the water-tank level |
| 1 | Pololu Dual VNH5019 motor shield | Dual H-bridge, 12 A per channel |
| 2 | DC gear motors + wheels | Differential drive |
| 1 | 2-channel 5 V relay module (opto-isolated) | Water pump (NO contact) + UVA grow lights (NC contact) |
| 1 | Submersible pump + tank + tubing | Irrigation |
| 1 | UVA grow-light bar | Light top-up |
| 1 | DS3232 RTC module (I2C 0x68, CR2032 backup) | Wall-clock time for the care scheduler |
| 1 | LDR photoresistor + divider | Ambient light (A1) |
| 1 | Resistive soil-moisture fork | Soil humidity (A0) |
| 1 | BME680 (I2C 0x76) | Ambient temperature/humidity/pressure/VOC |
| — | Resistive divider on Mega-TX1 to ESP32-RX2 | 5 V to 3.3 V level shifting |
| — | Common ground between Mega, motor shield and ESP32 | Mandatory |

The wiring single source of truth is [`docs/PINS - Sheet1.csv`](docs/PINS%20-%20Sheet1.csv).

### Software

- **PlatformIO** (CLI or VS Code extension) — builds the three firmware
  targets (`megaatmega2560`, `esp32dev`, `esp32cam`)
- **Python 3.10+** — protobuf/nanopb code generation (`infra/smartvase-proto/`)
  and the offline vision tools (`vision/requirements.txt`)
- **g++** — native unit-test suite in `tests/host/` (no hardware required)
- **Node.js 20+** — only for the image-upload Cloud Function
- **Doxygen 1.10 + Graphviz** — optional, to build the API documentation locally
- A serial terminal at 115200 baud (newline-terminated) for the on-device CLIs

All third-party code used by the firmware is vendored in the repository
(nanopb runtime, FirebaseClient/ESP_SSLClient for the CAM, local HC-SR04 and
DS3232 drivers): the build does not download anything.

## 3. Project Layout

```
smartvase/
├── README.md
├── Doxyfile                          # single Doxygen config (CI publishes to Pages)
├── build_mega.bat / build_hub.bat / build_cam.bat
├── docs/
│   ├── ARCHITECTURE.md               # full architectural reference
│   ├── SmartVase_Project_State.md    # current status and open items
│   ├── Plant_Care_Design.md          # autonomous-care design (L0/L1/L2 layers)
│   ├── SmartVase_data_structure.md   # MQTT/Firestore JSON schemas
│   ├── Lab_Bringup_Checklist.md      # hardware bring-up procedure
│   ├── Hardware_Verification_Sheet.md
│   └── PINS - Sheet1.csv             # wiring single source of truth
├── firmware/
│   ├── 1_esp32-hub/…/                # ESP32 Hub (FreeRTOS tasks, NVS config,
│   │   ├── include/                  #   TLS, provisioning, OTA)
│   │   └── src/
│   ├── 2_platform-controller_mega/…/src/   # Mega: superloop, movement FSM,
│   │                                 #   sensor pipeline, pump/lights, EEPROM,
│   │                                 #   CLI, pure-logic policy headers
│   └── 3_esp32-cam/…/                # CAM: camera driver, edge vision,
│       ├── lib/                      #   vendored FirebaseClient + ESP_SSLClient
│       └── src/
├── infra/
│   ├── smartvase-proto/              # canonical smartvase.proto + nanopb + sync hook
│   └── cloud-functions/upload-image/ # Cloud Function (Node 20): JPEG to Storage
├── server/                           # dev MQTT-to-Firestore bridge (Python)
├── vision/                           # offline vision prototyping + pytest suite
└── tests/host/                       # native unit tests (g++)
```

Each firmware is an independent PlatformIO project. The serial wire format is
schema-first: one canonical `.proto`; the generated sources are synced into
both firmwares by a pre-build hook (`infra/smartvase-proto/sync_to_firmware.py`).

## 4. How to Build, Burn and Run

### Build

From the repository root:

```bat
build_mega.bat    REM Arduino Mega 2560  (env: megaatmega2560)
build_hub.bat     REM ESP32 Hub          (env: esp32dev)
build_cam.bat     REM ESP32-CAM          (env: esp32cam)
```

Each wrapper calls `pio run` on the corresponding project under `firmware/`.
Note for the Hub: copy `src/secrets.h.example` to `src/secrets.h` first
(gitignored; used only as a bench fallback when NVS is empty).

### Burn (flash)

Connect the board over USB, find the port with `pio device list`, then:

```bat
build_mega.bat -t upload
build_hub.bat  -t upload
build_cam.bat  -t upload --upload-port COM5
```

The Mega and the Hub dev board auto-reset into their bootloaders (DTR/RTS).
The ESP32-CAM has no USB bridge on board: it needs a USB-UART adapter with
GPIO0 held low at reset (download mode).

### Run

1. **Mega bench bring-up:** open a serial monitor at 115200 (newline
   terminator) and type `standalone on` to suspend the Hub deadman, then
   calibrate with `calib`, `tank`, `light` (procedure in
   [`docs/Lab_Bringup_Checklist.md`](docs/Lab_Bringup_Checklist.md)).
2. **Hub provisioning:** from its CLI (`set wifi_ssid <...>`,
   `set wifi_pass <...>`, `set mqtt_broker <...>`, `save`, `reboot`) or via
   the `SmartVase_Setup_XXXX` access point that the Hub opens when it has no
   credentials (captive portal).
3. **CAM provisioning:** same CLI pattern (`set`, `save`, `reboot`),
   including the upload endpoint.
4. Power everything: the Hub heartbeats the Mega every 30 s, the Mega leaves
   degraded mode, and telemetry appears on `smartvase/{device_id}/telemetry`
   once per second.
5. Optionally enable autonomy from the Mega CLI: choose a plant profile
   (`plant sun|medium|shade`) and run `care on`. A freshly flashed robot
   never moves or waters on its own until this command is issued.

## 5. User Guide

### Operating modes

- `IDLE` — the robot stays put; grow lights follow the legacy rule (on when
  dark, within the 06:00–20:00 daylight window).
- `LIGHT` / `SHADOW` — the robot seeks brightness or shade while avoiding
  obstacles (proportional steering, escape sequence, stuck backoff).
- `care on` — the autonomous care layer runs the plant's day: morning light
  seek, basking with a daily light budget, heat-triggered shading,
  dose/soak/verify watering, end-of-day UVA top-up. A manual command always
  wins: care pauses for 30 minutes on operator override.

### MQTT interface

Topic root: `smartvase/{device_id}/`.

| Topic | Direction | Content |
|---|---|---|
| `telemetry` | Hub → cloud | JSON: distances, soil, lux, tank level, movement state, counters, care KPIs (1 Hz fast / 30 s deep) |
| `logs` | Hub → cloud | Structured events (INFO/WARN/ERROR/CRITICAL) |
| `alarm` | Hub → cloud | `mega_offline`, `mega_online`, `tx_queue_full`, … |
| `status` | broker (LWT) | `online`/`offline`, retained |
| `command/<type>` | app → Hub | JSON commands, see below |
| `command/ack` | Hub → cloud | Command outcome: status, detail, value, execution time |
| `vision/image` | CAM → cloud | Image metadata after each upload (URL, size, CRC32) |

Commands are JSON, for example:

```json
{ "type": "water", "cmd_id": 42, "duration_ms": 5000 }
```

Available types: `setMode`, `water`, `stop`, `requestDiagnostics`,
`setMotionParams`, `readSoil`, `softReset`. Every command is acknowledged on
`command/ack`; duplicate `cmd_id` values are detected and not re-executed
(safe retries). Watering is clamped to 30 s, rate-limited to one dose per
5 s, and refused when the tank looks empty.

### Serial CLIs (bench and provisioning)

- **Mega** (USB, 115200): `help`, `status`, `sensors`, `diag`, `i2cscan`,
  `mode <idle|light|shadow>`, `wall <left|right|off>`, `motor <f|b|l|r> <ms>`,
  `motortest`, `pump <ms>`, `tank <cm>`, `light <adc>`, `calib <l> <r>`,
  `plant [shade|medium|sun]`, `care [on|off]`, `rtc` / `rtc set <epoch>`,
  `stats`, `stats reset`, `standalone <on|off>`, `reboot`.
- **Hub** (USB, 115200): `diag` (Wi-Fi/MQTT/Mega-link health), `status`,
  `show`, NVS provisioning (`set`/`save`), plus passthrough commands to the
  Mega (`water`, `mode`, `soil`, `megadiag`, `telemetry`).
- **CAM** (USB, 115200): provisioning (`set`/`save`), `capture` (test without
  upload), `stats`, `version`.

### Built-in safety behavior

- Mega hardware watchdog (4 s) with a persistent reset counter.
- Degraded mode on low SRAM (<800 B, hysteresis at 1200 B) or when the Hub is
  silent for more than 120 s: motors and pump are forced off.
- The Hub declares `mega_offline` after 130 s of silence; the broker
  publishes the Hub's own `offline` status via MQTT Last Will.
- Pump: 60 s hard cap in the driver, tank guard active also during watering,
  glitch-free relay initialization at boot.

## 6. Software Architecture

- **Mega — cooperative superloop** (no RTOS, no `delay()` in the loop): every
  module is a non-blocking `tick()` driven by `millis()`; finite-state
  machines for movement (8 states), the serial RX parser, the care day cycle
  and the pump. Interrupt usage is minimal and deliberate: UART RX/TX ring
  buffers (raised to 256 B) and the timer behind `millis()`; a single
  critical section protects the log queue.
- **Hub — FreeRTOS**: `TaskSerialMega` (core 1, priority 3),
  `TaskMqttLink` (core 0, priority 2: TLS, NTP gate, reconnect backoff with
  jitter), `TaskMainLogic` (core 1, priority 1: JSON/Protobuf bridge,
  deadman, heartbeat). Tasks communicate only through bounded queues, with
  explicit alarms on saturation.
- **Wire protocol**: Protocol Buffers (nanopb, static allocation) inside the
  frame `0xAA | len(2) | payload | CRC16-CCITT(2)`, payload up to 256 B.
  Schema evolution has been done twice by appending tags (v4.0 → v4.2)
  without breaking older decoders. Since v4.2 the Hub also carries NTP time
  to the Mega inside the periodic heartbeat.
- **Data structures and algorithms**: median-of-3 + EMA sensor pipeline with
  NaN fail-safes, proportional differential steering with a seek bias
  (virtual-force-field composition), rotating 12-sector light scan with a
  single LDR, dual-slot wear-leveled EEPROM with CRC16, circular log queue
  with priority-aware backpressure, daily light budget as an auto-calibrated
  DLI proxy.

Detailed reference: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
Care design: [`docs/Plant_Care_Design.md`](docs/Plant_Care_Design.md).
JSON schemas: [`docs/SmartVase_data_structure.md`](docs/SmartVase_data_structure.md).

## 7. Testing

- **Native unit tests** (`tests/host/`, no hardware needed): 8 suites
  compiled with g++ against the real firmware sources through a minimal
  Arduino shim. Coverage: CRC16 protocol invariants (Mega and Hub must
  agree), command safety policies (clamps, rate limits), sensor fail-safes,
  proportional navigation (21 cases), EEPROM dual-slot wear leveling (with
  mocked `millis()` and EEPROM), the care decision table (about 50 checks)
  and whole-day simulations of the care layer at 1-minute ticks.

  ```bash
  bash tests/host/run.sh      # or tests\host\run.bat on Windows
  ```

- **Vision tests**: `cd vision && python -m pytest -q`.
- **Hardware bring-up**: incremental, guided procedure with dedicated CLI
  commands ([`docs/Lab_Bringup_Checklist.md`](docs/Lab_Bringup_Checklist.md),
  [`docs/Hardware_Verification_Sheet.md`](docs/Hardware_Verification_Sheet.md)).
- Problems found and fixed along the way (a CRC mismatch that silenced the
  serial link, an EEPROM slot bug that lost configuration on reboot, an
  8-bit counter overflow corrupting the light scan, a use-after-free in the
  CAM) are documented in
  [`docs/SmartVase_Project_State.md`](docs/SmartVase_Project_State.md).

## 8. Documentation

- **API documentation (Doxygen)**: [https://giacomoradin.github.io/SmartVase/](https://giacomoradin.github.io/SmartVase/).
  Generated from the in-source comments of all three firmwares. CI rebuilds
  and publishes it to GitHub Pages on every push to `main`
  ([`.github/workflows/generate-docs.yml`](.github/workflows/generate-docs.yml)).
  To build locally: `doxygen Doxyfile`, then open
  `docs/doxygen/html/index.html`. Includes per-module groups, call graphs
  and a browsable source cross-reference.
- **Design documents**: see [`docs/`](docs/).

## 9. Project Status

Firmware: Mega v5.4, Hub v1.4, CAM v2.2, serial protocol v4.2. All three
firmwares build offline and the host test suite passes. Hardware bring-up is
in progress: motor channel M1 has an intermittent direction wire, one side
ultrasonic sensor must be replaced, the RTC module is being replaced (its
failure is worked around in software: the Mega now receives NTP time from the
Hub with each heartbeat), and the field calibrations (soil thresholds, tank
depth, light-scan rotation time) are still to be done on the bench. Open
items are tracked in
[`docs/SmartVase_Project_State.md`](docs/SmartVase_Project_State.md).

## 10. Presentation and Demo Video

- Pitch video (2 min): [https://youtu.be/UZbBSWGEUTA](https://youtu.be/UZbBSWGEUTA)
- Presentation slides: [`docs/assets/SmartVaseProjectPresentation.pdf`](docs/assets/SmartVaseProjectPresentation.pdf)

## 11. Team Members and Contributions

| Member | Role | Contributions |
|---|---|---|
| **Giacomo Radin** | PM, Lead Firmware Engineer | System architecture; Mega platform-controller firmware (movement FSM, sensor pipeline, safety layer, EEPROM persistence, autonomous care layer, CLI); ESP32 Hub firmware (FreeRTOS tasks, MQTT/TLS, serial bridge); serial protocol design (Protobuf + framing); host test suite; documentation and Doxygen pipeline |
| **Antonio** | Computer Vision | ESP32-CAM edge vision (`VisionBotanist`: HSV metrics, leaf-health classification); Python prototyping and calibration tools (`vision/`) |
| **Fia** | Backend and Cloud | HiveMQ setup, image-upload Cloud Function, Firestore schema, MQTT-to-Firestore bridge |
| **Francesco** | Mobile App | Companion app (separate repository): Firestore integration, remote control and telemetry UI |

All members share responsibility for the system as a whole. Per-member
lines-of-code statistics are reported in the presentation.

## 12. License

This project is licensed under the MIT License — see [LICENSE](LICENSE).
