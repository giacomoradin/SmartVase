<div align="center">
  <img src="https://placehold.co/1200x400/1a2a1a/c2f0c2?text=SmartVase%0A%0AIoT%20Mobile%20Greenhouse&font=raleway" alt="SmartVase Project Banner">
</div>

<div align="center">
  <h3>A reliable, resilient, and observable IoT system for a mobile, fully automated greenhouse, controlled via a native Android application.</h3>
</div>

---

SmartVase is a mobile, self-watering planter that seeks light or shade
according to its operating mode, captures images of the plant for leaf-health
analysis, and reports telemetry and logs to a cloud backbone. The system is
distributed across three microcontrollers, a vision pipeline, and an
Android companion app.

For the full architectural reference (PIN map, FreeRTOS task layout,
state machines, EEPROM layout, MQTT topics) see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Table of Contents
1. [Project Vision and Foundational Principles](#1-project-vision-and-foundational-principles)
2. [System Architecture](#2-system-architecture)
3. [Communication Protocols](#3-communication-protocols)
4. [Key Firmware Concepts](#4-key-firmware-concepts)
5. [Developer Onboarding and Workflow](#5-developer-onboarding-and-workflow)
6. [Project Status](#6-project-status)
7. [License](#7-license)

---


## 1. Project Vision and Foundational Principles

The goal of SmartVase is to build a reliable IoT system that automates plant care.
The system architecture is based on four practical engineering principles:

- **Reliability and Fault Recovery**: The system operates continuously and recovers automatically from software freezes, sensor errors, and network disconnections.
- **System Monitoring and Diagnostics**: System status, sensor data, and structured logs are published over MQTT for real-time tracking.
- **Efficiency and Real-Time Control**: Uses lightweight binary serialization and non-blocking tasks to run smoothly on microcontrollers with limited memory.
- **Modular Architecture**: Clean separation between hardware drivers, communication logic, and cloud services to simplify testing and maintenance.

---

## 2. System Architecture

The system uses a distributed architecture that decouples responsibilities
across specialized hardware components and a cloud backbone.

### Firmware Subsystems

| Component       | Codename   | Core Function                                                            |
|-----------------|------------|--------------------------------------------------------------------------|
| Arduino Mega    | *The Brawn* | Direct hardware control: 6 ultrasonic sensors, Pololu Dual VNH5019 motors, pump relay, UVA grow-light relay, RTC |
| ESP32 Standard  | *The Brain* | Wi-Fi + TLS MQTT, JSON↔Protobuf bridge, coordination logic on FreeRTOS  |
| ESP32-CAM       | *The Eye*   | Periodic JPEG capture, onboard HSV vision analysis, direct upload to Firebase Storage & Firestore |
| Android App     | N/A        | User-facing control center (Kotlin, Compose, MVVM)                       |

### Cloud Backbone

```
ESP32 Hub  ⇄  HiveMQ Cloud (MQTT/TLS)  ⇄  Cloud Functions  ⇄  Firestore  ⇄  Android App
ESP32-CAM  ⇄  Firebase Storage & Firestore (Direct via HTTPS / C++ SDK)
```

Firestore is the authoritative store. For telemetry and commands, Cloud Functions act as the bridge between
the MQTT broker and Firestore documents. For image capture and leaf-health analysis, the ESP32-CAM connects directly
to Google Firebase Storage (to save JPEG images) and Firestore (to update `smartvase/{id}/vision/latest` documents
with foliage coverage and health metrics via REST/Patch API), bypassing MQTT for the vision pipeline.

### Vision Pipeline

The actual leaf-health analysis (quality gate, HSV color space metrics, circular Region of Interest (ROI) filtering,
foliage coverage, and green/brown ratio classifier) is executed directly on the edge in C++ within the
ESP32-CAM firmware (`firmware/3_esp32-cam/.../src/VisionBotanist.cpp`). The
Python script (`vision/pixel_analyzer.py`) and tests (`vision/tests/`) represent an earlier RGB565 differential
threshold prototype, maintained offline exclusively for prototyping and experimental calibration.

---

## 3. Communication Protocols

A hybrid strategy is used to balance interoperability and performance:

- **External (MQTT/JSON)**: All messages exchanged between Hub, Cloud, and App
  are JSON payloads on HiveMQ topics. Data schemas live in
  [`docs/SmartVase_data_structure.md`](docs/SmartVase_data_structure.md).
- **Internal (Serial/Protobuf)**: The Hub ↔ Mega link uses Protocol Buffers
  encoded with [nanopb](https://github.com/nanopb/nanopb) and wrapped in a
  custom framing `SOF | len | payload | CRC16-CCITT`. The schema is defined in
  [`infra/smartvase-proto/smartvase.proto`](infra/smartvase-proto/smartvase.proto).

### MQTT Topic Map (summary)

| Topic                                   | Direction                                | Description |
|-----------------------------------------|------------------------------------------|-------------|
| `smartvase/{id}/status`                 | Hub → Cloud → App                        | LWT connection status (`online` / `offline`, retained) |
| `smartvase/{id}/telemetry`              | Hub → Cloud → App                        | Periodic fast and deep telemetry JSON |
| `smartvase/{id}/logs`                   | Hub → Cloud                              | Structured log events (Firestore subcollection) |
| `smartvase/{id}/alarm`                  | Hub → Cloud → App                        | Critical system alarms and deadman events |
| `smartvase/{id}/command/#`              | App → Cloud → Hub                        | Incoming commands (`setMode`, `water`, `stop`, etc.) |
| `smartvase/{id}/command/ack`            | Hub → Cloud → App                        | Execution feedback and response values |

*(Note: The ESP32-CAM vision pipeline communicates directly with Firebase Storage and Firestore via HTTPS, bypassing MQTT).*

---

## 4. Key Firmware Concepts

- **Hardware Watchdog (WDT)**: The Arduino Mega resets automatically after 4 s
  of stall. The reset count is saved in EEPROM.
- **Degraded Mode**: Entered on low SRAM (<800 B) or when the Hub is silent
  for >120 s. In this mode, motors and pump are forced to a safe off state. Normal operation resumes when SRAM reaches ≥1200 B.
- **Hub Deadman Timer**: The Hub treats the Mega as offline after 130 s
  without telemetry or heartbeats and publishes an `alarm` event.
- **Data Persistence**: EEPROM dual-slot storage with magic numbers and
  CRC16 checks for both `DeviceConfig` and `CumulativeStats`. Writes are throttled
  to prevent memory cell wear.
- **Pump Safety Limits**: Maximum single irrigation duration is capped at 60 s
  by the `Pump` module on the Mega. As an additional safety measure, the Hub (`MainLogic`) limits any incoming MQTT watering command to 30 s before sending it to the Mega.
- **Time Synchronization and RTC Fallback (Proto v4.2)**: The Hub synchronizes its clock via NTP
  and attaches `epoch_s` to its periodic heartbeat sent to the Mega. If the Mega's hardware DS3232 RTC chip is disconnected or faulty, the Mega automatically uses a `millis()`-based software fallback clock synced to the Hub to maintain accurate timestamps.
- **Serial Framing**: Bytes outside a valid frame are ignored.
  CRC mismatches and protobuf decoding errors are tracked in system statistics.
- **FreeRTOS on the Hub**: Three dedicated tasks (Serial, MQTT, MainLogic)
  communicate exclusively through bounded queues, isolating Wi-Fi/MQTT latency
  from real-time serial communication.

---

## 5. Developer Onboarding and Workflow

### Prerequisites

- **PlatformIO** (CLI or IDE): required for all three firmware targets
- **Python 3.10+** + `nanopb` + `grpcio-tools`: required for regenerating Protobuf files
- **Python 3.10+** + dependencies in `vision/requirements.txt`: required for offline vision tests
- **Node.js 20+**: needed only if modifying the Cloud Function in `infra/cloud-functions/`
- **Android Studio**: needed only for the mobile app (tracked in a separate repository)

### Build the three firmwares

From the repository root:

```bash
build_mega.bat   # Arduino Mega 2560
build_hub.bat    # ESP32 Hub
build_cam.bat    # ESP32-CAM
```

Each script invokes `pio run -d <project>` against the appropriate
PlatformIO project under `firmware/`.



### Schema-First Workflow

The wire format is governed by `smartvase.proto`. To change it:

1. Edit [`infra/smartvase-proto/smartvase.proto`](infra/smartvase-proto/smartvase.proto).
2. Run [`generate_proto.bat`](infra/smartvase-proto/generate_proto.bat) to
   regenerate `smartvase.pb.{c,h}` with nanopb.
3. Verify the generated `smartvase.pb.h` includes `"pb.h"` (handled
   automatically by modern nanopb `v0.4.9.1`).
4. Copy the updated `.pb.{c,h}` into both firmware projects (Hub `include/`,
   Mega `src/`) and refresh `smartvase_aliases.h` if message/field names
   changed.
5. Use the generated, type-safe structures in firmware logic.

### Debug CLI (Mega)

Connect the Mega via USB at 115200 baud (Newline terminator):

```
--- SmartVase CLI ---
help                       this menu
version                    firmware version
status                     mode + runtime state (incl. growLight)
stats                      cumulative EEPROM stats
stats reset                clears cumulative EEPROM statistics
config                     current config
sensors                    latest sensor readings
diag                       guided sensor/motor diagnostics (incl. VNH5019 fault, UVA lights)
i2cscan                    hardware I²C bus scan (pins 20/21) with hints for RTC, EEPROM, and BME680
mode <idle|light|shadow>   set operating mode
plant [shade|medium|sun]   show / apply the plant profile preset
care [on|off]              autonomous plant care: status + daily KPIs / enable
wall <left|right|off>      local wall-following (overrides seeking)
motor <f|b|l|r> <ms>       motor test (max 60000 ms, wheels lifted)
mfp0                       continuous forward motor test (10 min) for electrical stress checks
motortest                  guided f/b/l/r sequence
pump <ms>                  pump test (max 60000 ms)
tank <cm>                  empty-tank threshold (US4)
light <adc>                light threshold 0..1023 (seeking + UVA grow lights)
calib <left> <right>       straight-drive PWM trim (0..255)
rtc / rtc set <epoch>      read / set the clock (software fallback if no DS3232)
standalone <on|off>        bench mode (suspends the Hub deadman)
reboot                     soft reset
```

---

## 6. Project Status

- **Firmware**: Mega v5.4.0, Hub v1.4.0, CAM v2.2.0 (serial protocol nanopb v4.2 with NTP time-sync).
  Aligned to the PIN map in [`docs/PINS - Sheet1.csv`](docs/PINS%20-%20Sheet1.csv).
  All three build offline. Hardware bring-up is in progress (motors and sensors
  partially verified on the bench, see `docs/SmartVase_Project_State.md`).
- **Autonomous Care (Mega v5.4.0)**: The robot can manage the plant's daily cycle autonomously
  (`care on`): daily light budget tracking, light-seeking relocation, automated
  dose/soak/verify watering, per-plant profiles, and UVA light top-up. Design document:
  [`docs/Plant_Care_Design.md`](docs/Plant_Care_Design.md).
- **Vision**: On-device leaf-health analysis (HSV color metrics, circular ROI, foliage coverage,
  green/brown ratios) is implemented in C++ on the ESP32-CAM (`VisionBotanist.cpp`) and uploads results directly
  to Firebase Storage and Firestore (`CloudService.cpp`). The `vision/` directory (`pixel_analyzer.py` and `tests/`)
  contains an offline RGB565 prototype used for experimental calibration.
- **Cloud**: Firestore is the primary database. The ESP32-CAM communicates directly with Firebase Storage and Firestore
  (`smartvase/{id}/vision/latest`). For MQTT telemetry and commands, a development MQTT-to-Firestore
  bridge is provided in `server/mqtt_firestore_bridge.py`.
- **Host Tests**: The `tests/host/` directory runs pure-logic unit tests with g++ (CRC16,
  command, sensor, navigation, care policies, and EEPROM persistence).
- **Android App**: Tracked in a separate repository.

Open items (HW-dependent or external) are tracked in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §10.

---

## 7. License

This project is licensed under the **MIT License**.
See the [LICENSE](LICENSE) file for full details.
