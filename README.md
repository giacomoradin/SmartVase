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
distributed across three microcontrollers, a Python vision pipeline, and an
Android companion app.

For the full architectural reference (PIN map, FreeRTOS task layout,
state machines, EEPROM layout, MQTT topics) see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## 🗺️ Table of Contents
1. [Project Vision & Foundational Principles](#-1-project-vision--foundational-principles)
2. [System Architecture](#-2-system-architecture)
3. [Communication Protocols](#-3-communication-protocols)
4. [Key Firmware Concepts](#-4-key-firmware-concepts)
5. [Team Roles & Responsibilities](#-5-team-roles--responsibilities)
6. [Developer Onboarding & Workflow](#-6-developer-onboarding--workflow)
7. [Project Status](#-7-project-status)
8. [License](#-8-license)

---

## 🎯 1. Project Vision & Foundational Principles

The objective of SmartVase is to engineer an IoT product that automates plant
cultivation with a high degree of reliability. The system's architecture is
predicated on four foundational principles:

- **Resilience & Robustness** — Operate autonomously for extended durations
  and recover gracefully from software stalls, hardware faults, and anomalous
  conditions.
- **Observability & Diagnostics** — Structured logs, granular telemetry, and
  health status reports surfaced through MQTT.
- **Performance & Efficiency** — Compact serialization and non-blocking
  execution models in resource-constrained environments.
- **Modularity & Maintainability** — Rigorous separation of concerns, enabling
  parallel development and long-term maintainability.

---

## 🏗️ 2. System Architecture

The system uses a distributed architecture that decouples responsibilities
across specialized hardware components and a cloud backbone.

### Firmware Subsystems

| Component       | Codename   | Core Function                                                            |
|-----------------|------------|--------------------------------------------------------------------------|
| Arduino Mega    | *The Brawn* | Direct hardware control: 6 ultrasonic sensors, Pololu Dual VNH5019 motors, pump relay, UVA grow-light relay, RTC |
| ESP32 Standard  | *The Brain* | Wi-Fi + TLS MQTT, JSON↔Protobuf bridge, coordination logic on FreeRTOS  |
| ESP32-CAM       | *The Eye*   | Periodic JPEG capture, HTTP upload to Cloud Function, MQTT publish      |
| Android App     | —          | User-facing control center (Kotlin, Compose, MVVM)                       |

### Cloud Backbone

```
ESP32 Hub / CAM  ⇄  HiveMQ Cloud (MQTT/TLS)  ⇄  Cloud Functions  ⇄  Firestore  ⇄  Android App / Vision pipeline
```

Firestore is the authoritative store. Cloud Functions act as the bridge between
the MQTT broker and Firestore documents, and host the image-upload endpoint
used by the ESP32-CAM (see `infra/cloud-functions/upload-image/`).

### Vision Pipeline

A Python script (`vision/pixel_analyzer.py`, owned by Antonio) downsamples a
frame to 160×120 RGB565 and counts green/brown pixels to produce a biomass /
disease index. The richer rule-based pipeline (quality gate, HSV metrics,
leaf-health classifier) targeted by the `vision/result` schema in
`SmartVase_data_structure.md` is **not implemented yet**.

---

## ⚡ 3. Communication Protocols

A hybrid strategy is used to balance interoperability and performance:

- **External (MQTT/JSON)** — All messages exchanged between Hub, Cloud, App
  and Vision are JSON payloads on HiveMQ topics. Canonical schemas live in
  [`SmartVase_data_structure.md`](SmartVase_data_structure.md).
- **Internal (Serial/Protobuf)** — The Hub ↔ Mega link uses Protocol Buffers
  encoded with [nanopb](https://github.com/nanopb/nanopb) and wrapped in a
  custom framing `SOF | len | payload | CRC16-CCITT`. Schema in
  [`infra/smartvase-proto/smartvase.proto`](infra/smartvase-proto/smartvase.proto).

### MQTT Topic Map (summary)

| Topic                                   | Direction                                |
|-----------------------------------------|------------------------------------------|
| `smartvase/{id}/telemetry`              | Hub → Cloud → App                        |
| `smartvase/{id}/logs`                   | Hub → Cloud (Firestore subcollection)    |
| `smartvase/{id}/alarm`                  | Hub → Cloud → App                        |
| `smartvase/{id}/command/#`              | App → Cloud → Hub                        |
| `smartvase/{id}/command/ack`            | Hub → Cloud → App                        |
| `smartvase/{id}/vision/image`           | CAM → Cloud → Vision                     |
| `smartvase/{id}/vision/result`          | Vision → Cloud → Hub & App               |

---

## 🛠️ 4. Key Firmware Concepts

- **Hardware Watchdog (WDT)** — Arduino Mega resets automatically after 4 s
  of stall. Reset count persisted in EEPROM.
- **Degraded Mode** — Entered on low SRAM (<800 B) or when the Hub is silent
  for >120 s; motors and pump are forced to a safe state. Exit gated by
  hysteresis (SRAM ≥1200 B).
- **Hub Deadman Timer** — The Hub treats the Mega as offline after 130 s
  without telemetry/heartbeat and publishes an `alarm` event.
- **Resilient Persistence** — EEPROM dual-slot writes with magic number and
  CRC16 for both `DeviceConfig` and `CumulativeStats`. Write-throttled
  to protect cell wear.
- **Pump Safety** — Maximum single irrigation session bounded to 60 s,
  enforced in the `Pump` module independently of the command sender.
- **Serial Framing** — Bytes outside a valid frame are dropped silently;
  CRC mismatches and protobuf decode failures are counted in stats.
- **FreeRTOS on the Hub** — Three pinned tasks (Serial / MQTT / MainLogic)
  communicate exclusively through bounded queues, isolating networking
  latency from the serial bridge.

---

## 👨‍💻 5. Team Roles & Responsibilities

| Member     | Role                                | Key Responsibilities                                                  |
|------------|-------------------------------------|----------------------------------------------------------------------|
| Giacomo    | PM & Lead Firmware Eng              | System architecture, firmware (ESP32 Hub & CAM, Mega)                |
| Antonio    | Computer Vision Specialist          | Image processing pipeline (Python OpenCV) and on-device pre-checks   |
| Fia        | Backend & Cloud Architect           | HiveMQ, Cloud Functions, Firestore schema                            |
| Francesco  | Android Application Developer       | Native Android app (Kotlin, Compose, MVVM, MQTT integration)         |

---

## 🚀 6. Developer Onboarding & Workflow

### Prerequisites

- **PlatformIO** (CLI or IDE) — for all three firmware targets
- **Python 3.10+** + `nanopb` + `grpcio-tools` — for regenerating Protobuf
- **Python 3.10+** + dependencies in `vision/requirements.txt` — for the vision pipeline
- **Node.js 20+** — only if you touch the Cloud Function in `infra/cloud-functions/`
- **Android Studio** — only for the mobile app (separate repository)

### Build the three firmwares

From the repository root:

```bash
build_mega.bat   # Arduino Mega 2560
build_hub.bat    # ESP32 Hub
build_cam.bat    # ESP32-CAM
```

Each script invokes `pio run -d <project>` against the appropriate
PlatformIO project under `firmware/`.

### Run the vision tests

```bash
cd vision
pip install -r requirements.txt
python -m pytest -q
```

### Schema-First Workflow

The wire format is governed by `smartvase.proto`. To change it:

1. ✍️ Edit [`infra/smartvase-proto/smartvase.proto`](infra/smartvase-proto/smartvase.proto).
2. ⚙️ Run [`generate_proto.bat`](infra/smartvase-proto/generate_proto.bat) to
   regenerate `smartvase.pb.{c,h}` with nanopb.
3. Patch the generated `smartvase.pb.h`: change `#include <pb.h>` to
   `#include "pb.h"`.
4. Copy the updated `.pb.{c,h}` into both firmware projects (Hub `include/`,
   Mega `src/`) and refresh `smartvase_aliases.h` if message/field names
   changed.
5. 💡 Use the generated, type-safe structures in firmware logic.

### Debug CLI (Mega)

Connect the Mega via USB at 115200 baud (Newline terminator):

```
--- SmartVase CLI ---
help                       this menu
version                    firmware version
status                     mode + runtime state (incl. growLight)
stats                      cumulative EEPROM stats
config                     current config
sensors                    latest sensor readings
diag                       guided sensor/motor diagnostics (incl. VNH5019 fault, UVA lights)
mode <idle|light|shadow>   set operating mode
motor <f|b|l|r> <ms>       motor test (max 60000 ms, wheels lifted)
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

## 📈 7. Project Status

- **Firmware** — Mega v5.2, Hub v1.3, CAM v2.1 (serial protocol nanopb v4.0).
  Aligned to the PIN map in [`docs/PINS - Sheet1.csv`](docs/PINS%20-%20Sheet1.csv).
  All three build offline; hardware bring-up still pending.
- **Vision** — Single script `vision/pixel_analyzer.py` (green/brown pixel
  analysis) with one test under `vision/tests/`. The full quality-gate /
  leaf-health pipeline is not implemented yet.
- **Cloud** — The image-upload Cloud Function is a stub; a dev MQTT→Firestore
  bridge lives in `server/mqtt_listener.py`. Production pipeline TBD.
- **Host tests** — `tests/host/` runs pure-logic unit tests with g++ (CRC16,
  command/sensor policies).
- **Android app** — Tracked in a separate repository.

Open items (HW-dependent or external) are tracked in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §10.

---

## 📜 8. License

This project is licensed under the **MIT License**.
See the [LICENSE](LICENSE) file for full details.
