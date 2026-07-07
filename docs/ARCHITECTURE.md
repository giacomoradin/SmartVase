# SmartVase: Architecture and Project Context

> Technical onboarding document for the SmartVase team.
> State consolidated as of **2026-07-02**. Aligned with the PIN map
> `docs/PINS - Sheet1.csv` and current architectural decisions.
> The behavioral and product design of the autonomous care layer is documented in
> `docs/Plant_Care_Design.md`.

---

## 1. Current Project Status (TL;DR)

- The hardware prototype is currently in **lab bring-up** (June to July 2026): motors and
  sensors are partially verified on the bench. The PIN map (`docs/PINS - Sheet1.csv`)
  serves as the primary wiring reference.
- Firmware versions (all build offline):
  - Mega: **v5.4.0**: 6 HC-SR04 ultrasonic sensors, pump via relay with tank protection, RTC
    DS3232 (+ software fallback clock synced via NTP), UVA grow lights, VNH5019 motor driver,
    proportional steering + wall-following, and the **autonomous plant-care
    layer** (`Care` + `CarePolicy.h`: daily light budget, rotating light scan,
    dose/soak/verify watering, plant profiles: see
    `docs/Plant_Care_Design.md`). SRAM hysteresis (800/1200 B) prevents rapid toggling in degraded mode.
  - Hub: **v1.4.0**: publishes telemetry, logs, alarms, and **command/ack**;
    deadman switch active; TLS connection to HiveMQ with NTP time sync; non-blocking MQTT
    reconnect; OTA updates (ready for bench validation); AP provisioning + captive portal.
  - ESP32-CAM: **v2.2.0**: Wi-Fi STA + NTP + direct HTTPS upload to Google Firebase Storage, plus on-device C++ leaf-health analysis (`VisionBotanist.cpp`) that writes health metrics directly to Firestore (`smartvase/{id}/vision/latest`).
  - Protocol: **proto v4.2**: `TelemetryFast` with 5 nav distances
    + soil moisture + epoch_s; `CommandResponse` with a `value` field;
    `TelemetryDeep` extended with autonomous-care daily KPIs
    (state, light-budget %, relocations, doses, UVA minutes: tags 22 to 27,
    published by the Hub as the `care` JSON object). Includes Hub-to-Mega NTP time synchronization.
- **Vision pipeline**: on-device leaf-health analysis (HSV color metrics, quality gate,
  circular ROI, foliage coverage, green/brown ratios) is implemented in C++ on the ESP32-CAM
  (`VisionBotanist.cpp`). The Python scripts in `vision/` represent an earlier RGB565 differential prototype used offline for prototyping, testing, and calibrating the edge algorithm.
- **Cloud Function stub** `upload-image` is located in `infra/cloud-functions/`
  (Node 20 + Firebase Storage). Note that the ESP32-CAM currently bypasses this function and uploads directly to Firebase Storage via C++ SDKs.

---

## 2. Product Vision

SmartVase is a **mobile, autonomous IoT planter and greenhouse**:

- It moves on wheels seeking light or shade, either on command
  (`LIGHT`, `SHADOW`, `IDLE` modes) or **autonomously**: with the care layer
  enabled (Mega v5.4.0, `care on`), the robot manages the plant's daily cycle,
  maintaining a daily **light budget** and keeping soil moisture within the range of
  the selected plant profile (design: `docs/Plant_Care_Design.md`).
- It waters the plant on command (relay-controlled pump) or autonomously in
  **dose/soak/verify** cycles based on soil moisture (fork sensor), always
  enforcing tank-protection and maximum duration limits.
- It periodically captures images of the plant with the ESP32-CAM and evaluates
  frame quality and leaf health via the onboard C++ vision algorithm.
- It is controlled by an **Android app** (MVVM + Compose, developed by Francesco).
- It reports telemetry and logs to HiveMQ Cloud, with Firestore acting as the primary database.

Project engineering principles:
**Reliability and Fault Recovery** · **System Monitoring and Diagnostics** ·
**Efficiency and Real-Time Control** · **Modular Architecture**.

---

## 3. Hardware Topology

The system consists of three microcontrollers and a mobile application:

| Role                 | MCU             | Codename     | Task                                                                    |
|----------------------|-----------------|--------------|-------------------------------------------------------------------------|
| Platform Controller  | Arduino Mega    | *The Brawn*  | Direct control of motors, pump, sensors, RTC. No networking.            |
| Logic & Web Hub      | ESP32 standard  | *The Brain*  | Wi-Fi, MQTT/TLS to HiveMQ, coordination, JSON↔Protobuf bridge.          |
| Vision Co-Processor  | ESP32-CAM       | *The Eye*    | JPEG capture, on-edge C++ leaf-health analysis, direct Firebase upload. |
| Android App          | N/A             | N/A          | User UI (under local development, tracked separately).                  |

### 3.1 Communication Buses

- **Hub ↔ Mega**: serial UART (115200 baud). Frame structure:
  `SOF=0xAA | len_hi | len_lo | payload(protobuf) | crc16_hi | crc16_lo`.
  Payload is `WrapperMessage` from [smartvase.proto](infra/smartvase-proto/smartvase.proto).
- **Hub ↔ Cloud**: MQTT over TLS to **HiveMQ Cloud** using **JSON** payloads.
- **Cloud pipeline**:
  `HiveMQ ⇄ Cloud Functions ⇄ Firestore ⇄ Android App`.
  Firestore is the primary database. The Cloud Functions bridge MQTT and
  Firestore documents (see [SmartVase_data_structure.md](SmartVase_data_structure.md)).
- **CAM ↔ Cloud**: **direct HTTPS connection**. The ESP32-CAM connects to Wi-Fi, captures images,
  performs on-edge C++ leaf-health analysis (`VisionBotanist.cpp`), uploads JPEG files directly to Firebase Storage,
  and updates leaf health metrics directly on Firestore (`smartvase/{id}/vision/latest`).
- **Python Vision**: used offline in `vision/` for prototyping and calibrating
  the C++ edge algorithm.

---

## 4. Authoritative PIN Map (Arduino Mega)

From the file [docs/PINS - Sheet1.csv](docs/PINS - Sheet1.csv).
**This table takes priority over any `#define` present in the current firmware
sources.** All Mega firmware must be aligned to these pins.

### 4.1 HC-SR04 Ultrasonic Sensors (6 total)

| ID  | Physical Role                      | Trigger | Echo | Notes                                  |
|-----|------------------------------------|---------|------|----------------------------------------|
| US1 | Front-top (forward, high)          | D33     | D35  | Upper front anti-collision             |
| US2 | Front-right                        | D26     | D27  | Front-right anti-collision             |
| US3 | Front-left                         | D36     | D37  | Front-left anti-collision              |
| US4 | Water tank                         | D50     | D51  | **Measures the water level in the tank** |
| US5 | Left side                          | D4      | D5   | Left-side anti-collision               |
| US6 | Right side                         | D28     | D29  | Right-side anti-collision              |

US1, US2, US3, US5, US6: dedicated to **navigation and obstacle avoidance** (5 sensors).
US4: dedicated **only** to measuring the water level in the tank.

### 4.2 Motor Driver: Pololu Dual VNH5019 Shield (2 DC motors)

The motor shield is a **Pololu Dual VNH5019** (`ash02b`, 2014), wired to the Mega
with jumpers. Interface per motor: `INA`/`INB` (direction),
`PWM` (speed, PWM pin), `EN/DIAG` (enable + fault flag, open-drain with a
pull-up on the shield: driver enabled at rest).

| Shield Signal  | Mega Pin | Notes |
|----------------|----------|-------|
| M1PWM          | D7 (PWM) | Left motor speed |
| M1INA          | D41      | Direction |
| M1INB          | D43      | Direction |
| M1EN/DIAG      | Not wired | Optional, not connected as of 2026-06-30 |
| M2PWM          | D6 (PWM) | Right motor speed |
| M2INA          | D45      | Direction |
| M2INB          | D47      | Direction |
| M2EN/DIAG      | Not wired | Optional, not connected as of 2026-06-30 |

> Pin mapping **confirmed 2026-06-30** via multimeter continuity test (see
> `docs/Hardware_Verification_Sheet.md` §1.1/T7), replacing an earlier
> mapping that paired PWM/INA/INB from different shield channels.
>
> [Movement.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Movement.cpp)
> drives PWM (`analogWrite`) and INA/INB (`digitalWrite`). EN/DIAG is not wired to
> the Mega (`MOTOR_EN_DIAG_WIRED 0`): `faultLeft()` and `faultRight()` always
> return `false` until physically wired and configured.
> Which motor corresponds to left and wheel directions must be verified on the
> bench using `motortest`.
> Note: **Common GND** between Mega and shield is mandatory: without a shared ground reference,
> signals are not received and outputs remain at 0 V even when power is connected.

### 4.3 Power and RTC

| Function                       | Mega Pin       | Notes                                                  |
|--------------------------------|----------------|-------------------------------------------------------|
| Battery voltage divider        | A2 (`BATTERY_MONITORING_ENABLED=0`) | Divider R1=30k, R2=7.5k (`Vbatt = Vadc · 5.0`), not mounted |
| RTC DS3232: SDA                | D20 (SDA)      | I²C 0x68                                              |
| RTC DS3232: SCL                | D21 (SCL)      | I²C 0x68                                              |

> **RTC Software Fallback Clock and Hub NTP Sync**: If the DS3232 chip does not
> respond on I²C or has a stopped oscillator, the Mega runs a `millis()`-based
> software clock. At boot, if no valid time is available, it defaults to
> **08:00** (`DEFAULT_BOOT_HOUR`, inside the grow-light daylight window). Since
> proto v4.2, the software clock is **re-synchronized by every Hub heartbeat** carrying
> the Hub's NTP epoch (~30 s period, see §5.1). When connected to the Hub, the Mega
> tracks real time with negligible drift without needing manual `rtc set` commands. Standalone
> bench sessions still use `rtc set <epoch>`. While active, the software clock takes precedence over a faulty hardware chip.
> Note on Bench Status (2026-07-06): The HW-084 module (DS3231) is **faulty**. The I²C bus is healthy (BME680 at 0x76 answers via `i2cscan`), but both chips on the RTC module are silent. The Hub NTP sync acts as the primary time source until the module is replaced.

### 4.4 Pump, Relays, and Grow Lights

| Function               | Mega Pin | Notes                                |
|------------------------|----------|-------------------------------------|
| Relay channel 1 (IN1)  | D10      | Irrigation pump (active LOW, `Pump` module) |
| Relay channel 2 (IN2)  | D11      | **UVA grow lights** (`GrowLight` module) |

- **Pump** ([Pump.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Pump.cpp)):
  relay D10 active-LOW, 60 s duration limit, empty-tank protection (US4). The
  `WaterCommand` is implemented end-to-end.
- **UVA lights** ([GrowLight.cpp](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/GrowLight.cpp)):
  relay D11, lights wired on the **NC** contact (with the relay at rest they are
  ON, polarity inverted compared to the pump). Two driving policies:
  - **care layer active** (v5.4.0): the lights act as an **end-of-day budget
    top-up**, turning on near the end of the daylight window if the daily
    light budget is in deficit, subject to a daily limit (`CARE_TOP_UP` state,
    see `docs/Plant_Care_Design.md` §6);
  - **care layer off** (legacy rule): turns on only if in `IDLE` mode **and**
    `lux < light_threshold` **and** within the daylight window **06:00 to 20:00**
    (gated via RTC or software clock). Logic defined in
    [`growLightWanted()` and `withinDaylightWindow()`](firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/SensorPolicy.h).

### 4.5 "Fork" Soil-Moisture Sensor

| Function           | Pin       | Notes                                               |
|--------------------|-----------|-----------------------------------------------------|
| Fork (signal)      | A0        | Two-prong probe for soil moisture                   |
| Fork VCC           | 5V        | Power supply                                        |
| Fork GND           | GND       | Ground reference                                    |

> **Photoresistor vs. fork conflict on A0**: The CSV file maps both sensors to A0, but on the
> Mega each ADC input is single-ended. Architectural decision: **the fork remains on A0, and the
> photoresistor moves to another free analog pin** (e.g., A1 or A3). This change will be finalized during hardware refactoring.

### 4.6 Photoresistor (Ambient Light)

| Function           | Pin       | Notes                                               |
|--------------------|-----------|-----------------------------------------------------|
| Photoresistor (LDR)| **TBD**   | Originally on A0 in legacy firmware. To be reassigned to accommodate the fork sensor. |

The photoresistor drives the `LIGHT` and `SHADOW` state machines:
the robot turns right if more light is needed, and left if shade is needed.

### 4.7 Mega Pins Currently Unassigned or Unclear

- BME680 sensor (Temperature, Relative Humidity, Pressure, VOC): used in legacy firmware via
  I²C `0x76`, but **not present in the CSV wiring map**. To be clarified if it remains in the bill of materials.
- Motor current sensor (INA219, roadmap): planned for future monitoring, not currently wired.

---

## 5. Data Protocol

### 5.1 Hub ↔ Mega Serial (Protobuf, nanopb)

Source file: [infra/smartvase-proto/smartvase.proto](infra/smartvase-proto/smartvase.proto).

Atomic messages wrapped in `WrapperMessage`:

- **TelemetryFast** (high-frequency send):
  `front_dist_cm`, `left_dist_cm`, `right_dist_cm`, `water_level_cm`, `lux`,
  `movement_state`, `device_id`.
  Note: After refactoring to 6 ultrasonic sensors, **this schema must be extended**
  with `front_top_dist_cm` (US1) and `front_right_dist_cm` / `front_left_dist_cm` (US2/US3).
- **TelemetryDeep** (low-frequency send, ~minutes):
  BME680 (`temperature_c`, `humidity_percent`, `pressure_hpa`,
  `gas_resistance_ohms`), `uptime_s`, `free_ram_bytes`, cumulative counters
  (`watchdog_resets`, `total_irrigations`, `obstacles_avoided`, `stuck_events`,
  `bme_read_errors`, `log_overflows`), and `battery_voltage`.
- **Log**: `level` (INFO/WARN/ERROR/CRITICAL), `event`, `detail`,
  `timestamp_ms`, and `source_device`.
- **Heartbeat**: `uptime_s`, `is_degraded`, `device_id`, and `epoch_s`
  (proto v4.2). In the Hub-to-Mega direction, the heartbeat serves as the
  Mega's **time source**: the Hub attaches its NTP epoch in UTC (`0` indicates no
  NTP time yet, which is ignored), allowing the Mega to synchronize its software clock.
  This ensures accurate timekeeping even when the hardware RTC module is offline or faulty.
  Timezone offsets are applied on the Mega (`HUB_EPOCH_TZ_OFFSET_S`, Sensors.cpp).
- **Command** (Hub to Mega, oneof field): `water`, `set_mode`, `stop`,
  `request_diagnostics`, `set_motion_params`, `read_soil`, or `soft_reset`.
- **CommandResponse** (Mega to Hub): `status` (OK/ERROR), `detail`,
  `cmd_id`, and `exec_time_ms`.

Code generation: see [infra/smartvase-proto/generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
Note: Modern nanopb (`v0.4.9.1`) automatically generates `#include "pb.h"` in `smartvase.pb.h`, so manual patching is no longer required.

### 5.2 MQTT (JSON)

Root topic: `smartvase/{device_id}/...`. Full specification available in
[SmartVase_data_structure.md](SmartVase_data_structure.md). Summary of active topics:

| Topic                                | Direction                              | Description |
|--------------------------------------|----------------------------------------|-------------|
| `smartvase/{id}/status`              | Hub → Cloud → App                      | LWT connection status (`online` / `offline`, retained) |
| `smartvase/{id}/telemetry`           | Hub → Cloud → App                      | Periodic fast and deep telemetry JSON |
| `smartvase/{id}/logs`                | Hub → Cloud                            | Structured log events (Firestore subcollection) |
| `smartvase/{id}/alarm`               | Hub → Cloud → App                      | Critical system alarms and deadman events |
| `smartvase/{id}/command/config`      | App → Cloud → Hub                      | Configuration update commands |
| `smartvase/{id}/command/#`           | App → Cloud → Hub                      | Incoming commands (`setMode`, `water`, `stop`, etc.) |
| `smartvase/{id}/command/ack`         | Hub → Cloud → App                      | Command execution feedback (`status`, `value`, `exec_time_ms`) |

*(Note: The ESP32-CAM vision pipeline communicates directly with Firebase Storage and Firestore via HTTPS, bypassing MQTT).*

---

## 6. Repository Structure

```
SmartVase/
├── README.md                              # Public overview
├── SmartVase_data_structure.md            # MQTT and Firestore JSON specification
├── docs/
│   ├── ARCHITECTURE.md                    # This file
│   └── PINS - Sheet1.csv                  # Authoritative PIN wiring map
├── build_hub.bat / build_mega.bat / build_cam.bat   # PlatformIO build wrapper scripts
├── infra/
│   ├── hivemq_ca_cert.h                   # HiveMQ CA certificate shared by Hub and CAM
│   ├── smartvase-proto/                   # Protobuf definitions and build scripts
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
│   ├── lib/                               # Local library archives (DriverDkv, HCSR04)
│   ├── 1_esp32-hub/
│   │   └── Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/
│   │       ├── platformio.ini             # esp32dev + ArduinoJson + PubSubClient + AsyncTCP
│   │       ├── include/                   # Headers, smartvase.pb.h, and nanopb runtime
│   │       └── src/                       # ConfigManager, WifiManager, SerialManager, MqttManager, MainLogic
│   ├── 2_platform-controller_mega/
│   │   ├── README_PlatformController_Arduino.md
│   │   └── Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/
│   │       ├── platformio.ini             # megaatmega2560 + BME680 + HCSR04 + DS3232RTC
│   │       └── src/                       # main.cpp, Movement, Sensors, Communication, Persistence, Pump, Cli
│   └── 3_esp32-cam/
│       └── Radin_Giacomo_SmartVase_VisionCoProcessor_ESP32CAM/
│           ├── platformio.ini
│           └── src/                       # main.cpp, CameraDriver, CloudService, VisionBotanist, ConsoleCLI
└── vision/
    ├── requirements.txt                   # Python dependencies (opencv, numpy, pytest)
    ├── vision/
    │   ├── __init__.py
    │   ├── quality_gate.py                # Brightness and Laplacian variance checks
    │   ├── metrics.py                     # HSV ratios, dominant color, bounding box
    │   ├── leaf_health.py                 # Rule-based classifier
    │   └── pipeline.py                    # End-to-end pipeline
    └── tests/
        ├── test_quality_gate.py
        ├── test_metrics.py
        ├── test_leaf_health.py
        └── test_pipeline.py
```

---

## 7. Firmware Architecture (Current State)

### 7.1 Mega: `2_platform-controller_mega`

- **`main.cpp`**: Setup and non-blocking loop, hardware WDT (4 s), reset recovery
  with `watchdog_resets` counter, and `degradedMode` handling triggered when `freeRam < 800`
  or when the Hub is silent for > 120 s.
- **`Movement`**: State machine controlling navigation (`M_IDLE → M_MOVING → M_AVOID_START →
  M_AVOID_REVERSING → M_AVOID_TURNING → M_STUCK`, plus internal light-scan states).
  In `M_MOVING`, it uses **proportional differential steering** (`NavPolicy.h`) instead of
  stop-and-turn: steering continuously away from closer obstacles, speed ramping down in close proximity,
  and reverse/turn recovery for emergency distances. Light/shadow seeking (`light_threshold`,
  default **500**, tunable via `light <adc>`) acts as a gentle steering bias.
  **Rotating light scan** (v5.4.0): rotates in place while recording ADC averages across 12 sectors,
  then turns to the brightest or darkest sector before moving forward. Optional
  **wall-following** sub-mode (`wall <left|right|off>`) uses side ultrasonic sensors.
  All motor commands are routed through `driveMotors(left,right)`. Driver: **Pololu Dual VNH5019** (PWM/INA/INB).
- **`NavPolicy.h`**: Pure, host-testable navigation logic without hardware dependencies:
  `proportionalDrive` and `wallFollowDrive`. Unit-tested in `tests/host/test_nav_policy.cpp`.
- **`Care` / `CarePolicy.h`** (v5.4.0): The **autonomous plant-care layer**
  (`CarePolicy.h` is pure and host-testable in `tests/host/test_care_policy.cpp`).
  Manages plant profiles (shade, medium, sun presets), daily **light budget** tracking,
  sector selection, dose/soak/verify watering decisions, and the daily state machine
  (`NIGHT → SEEK_SUN → BASK → SEEK_SHADE/SHELTER → TOP_UP`). `Care.cpp` executes
  at 1 Hz and updates actuators, tracking daily KPIs (budget %, doses, relocations, UVA minutes).
  Includes manual override suspension (pauses autonomy for 30 min on user command).
  Configured via `DeviceConfig` in EEPROM; **disabled by default**, enabled with `care on`.
- **`Sensors`**: Round-robin reading of **6** HC-SR04 ultrasonic sensors using a **median-of-3
  anti-bounce pre-filter** (`medianOf3`) feeding an EMA filter (α=0.4), with validity thresholds (2 to 200 cm).
  Manages RTC DS3232 and the **software fallback clock** synced via NTP. BME680 and battery monitoring are disabled by default.
- **`Pump`**: Controls relay D10 (active-LOW) with a 60 s maximum duration limit and empty-tank protection (US4).
- **`GrowLight`**: Controls relay D11 (UVA lights wired to NC contact). When the care layer is active,
  lights act as an end-of-day top-up if the light budget is in deficit. When care is off, legacy rules apply:
  turns on in IDLE mode when ambient light is low during the daylight window (06:00 to 20:00).
- **`Communication`**: Serial framing (SOF/len/payload/CRC16-CCITT),
  circular log queue (20 slots), Protobuf encoding/decoding, command idempotency, and rate limiting (`CommandPolicy.h`).
- **`Persistence`**: **Dual-slot** EEPROM (`SLOT_0` / `SLOT_1`) with
  wear leveling rotation, magic numbers, and CRC16 validation for `DeviceConfig`
  and `CumulativeStats`. Writes are throttled (60 s for config, 300 s for stats).
- **Debug CLI**: Available over USB at 115200 baud. Commands: `status`, `stats`, `stats reset`,
  `config`, `sensors`, `diag`, `i2cscan`, `reboot`, `mode`, `plant [shade|medium|sun]`,
  `care [on|off]`, `wall <left|right|off>`, `motor <dir> <ms>`, `mfp0`, `motortest`,
  `pump <ms>`, `tank <cm>`, `light <adc>`, `calib`, `rtc`, `standalone`, `help`.

### 7.2 ESP32 Hub: `1_esp32-hub`

**FreeRTOS** architecture with 3 pinned tasks and 4 queues:

| Task           | Core | Priority | Stack | Role                                        |
|----------------|------|----------|-------|---------------------------------------------|
| `TaskSerialMega` | 1  | 3 (high) | 4 KB  | UART2 ↔ Mega, Protobuf+framing encode/decode |
| `TaskMqttLink`   | 0  | 2 (med)  | 8 KB  | TLS connection to HiveMQ, pub/sub           |
| `TaskMainLogic`  | 0  | 1 (low)  | 8 KB  | JSON↔Protobuf bridge, telemetry timer       |

Queues (`xQueueCreate`):
- `serialRxQueue` (Mega to MainLogic): `SerialMessage` wrapping `WrapperMessage`.
- `serialTxQueue` (MainLogic to Mega): Outbound Protobuf messages.
- `mqttTxQueue`   (MainLogic to MqttManager): `MqttMessage{topic,payload}`.
- `mqttRxQueue`   (MqttManager to MainLogic): `MqttCommand{topic,payload,ts}`.

Modules:
- **`ConfigManager`**: NVS persistence for `DeviceConfig` (WiFi, MQTT, webhook credentials) with CRC16 validation.
- **`WifiManager`**: Wi-Fi Station connection with **provisioning Access Point** fallback if credentials are unset or connection fails.
- **`SerialManager`**: Manages Serial2 on `MEGA_RX_PIN=16 / MEGA_TX_PIN=17`.
- **`MqttManager`**: Manages HiveMQ TLS connection, MAC-based client ID, LWT on `smartvase/HUB_{macSuffix}/status`, and subscriptions to `smartvase/HUB_{macSuffix}/command/#`.
- **`MainLogic`**: 60 s telemetry timer and `checkMegaConnection` deadman switch (130 s timeout). Routes incoming MQTT commands to Protobuf serial messages (`setMode`, `water`, `stop`, `requestDiagnostics`, `setMotionParams`, `readSoil`, `softReset`). Also enforces a preventive 30 s safety cap on incoming MQTT watering commands.

### 7.3 ESP32-CAM: `3_esp32-cam`

Current state (v2.2.0): **Production edge vision firmware**.
Connects to Wi-Fi, synchronizes time via NTP, and performs onboard JPEG capture.
- **`CameraDriver`**: Configures OV2640 camera frame buffer and resolution.
- **`VisionBotanist`**: Converts JPEG frames to RGB888, applies circular Region of Interest (ROI) filtering, transforms colors to HSV floating-point space, and calculates foliage coverage and green/brown ratios.
- **`CloudService`**: Uploads JPEG images directly to Google Firebase Storage (`gs://...`) using C++ SDKs and updates health analysis results directly on Firestore (`smartvase/{id}/vision/latest`) via REST Patch API.

### 7.4 Vision Pipeline: C++ Edge vs. Python Prototyping

- **On-Edge C++ Analysis (`firmware/3_esp32-cam/.../src/VisionBotanist.cpp`)**: Executes
  leaf-health analysis directly on the ESP32-CAM using HSV color space metrics and circular ROI filtering.
- **Python Prototyping (`vision/`)**: Contains `pixel_analyzer.py` and `tests/test_analyzer.py`,
  representing an earlier RGB565 differential threshold prototype used offline for testing and experimental calibration.

---

## 8. Resilience and Observability (Key Concepts)

- **Hardware Watchdog** (Mega): WDTO_4S timer with persisted reset count in EEPROM.
- **Low Memory Protection**: Enters `degradedMode` if `freeRam < 800 B`.
- **Hub Deadman Timer**: The Mega enters `degradedMode("Hub Missing")` if it
  receives no serial messages from the Hub for >120 s. The Hub treats the Mega as offline after 130 s (10 s margin).
- **Data Persistence**: Dual EEPROM slots with magic numbers and CRC16 validation, falling back to
  defaults if data corruption is detected.
- **Serial Framing**: SOF + len + CRC16 (poly 0x1021) framing with a decoding state
  machine that discards malformed bytes.
- **Structured Logs**: INFO, WARN, ERROR, and CRITICAL log levels managed via a circular queue
  to prevent blocking during high traffic (buffer overflows are tracked in system stats).

---

## 9. Development Workflow

### 9.1 Prerequisites

- PlatformIO (CLI recommended, used by build `.bat` scripts).
- Python 3.x + `nanopb` (`pip install nanopb`) + `protoc`.
- For offline vision tests: `pip install -r vision/requirements.txt`.
- Android Studio (for mobile app development, outside this repo).

### 9.2 Build

From the project root:

```
build_mega.bat   # Arduino Mega
build_hub.bat    # ESP32 Hub
build_cam.bat    # ESP32-CAM
```

The batch files invoke `pio run -d <project>`.

### 9.3 Schema-First Workflow

1. Edit [smartvase.proto](infra/smartvase-proto/smartvase.proto).
2. Run [generate_proto.bat](infra/smartvase-proto/generate_proto.bat).
3. Verify the generated `smartvase.pb.h` includes `"pb.h"` (handled automatically by modern nanopb `v0.4.9.1`).
4. Copy updated `.pb.{c,h}` files into `firmware/1_esp32-hub/.../src+include`
   and `firmware/2_platform-controller_mega/.../src`.
5. Update `smartvase_aliases.h` if message, enum, or tag names changed.



---

## 10. Open Items and TODO

The following items remain open for hardware verification and refinement, ordered by priority:

### A. Hardware Verification and Testing (High Priority)

1. **Firmware Flashing and Integration**: Flash the three firmware builds onto physical hardware using `build_mega.bat`, `build_hub.bat`, and `build_cam.bat` once wiring is complete.
2. **Pump Relay Polarity**: In `Pump.cpp`, `PUMP_RELAY_ACTIVE_LOW` is set to `1`. If the physical relay module is active-high, change this definition to `0`.
3. **VNH5019 Motor Driver**: PWM/INA/INB pin mapping was confirmed via continuity tests on 2026-06-30. Bench verification required: ensure **common GND** between Mega and shield, and test motor movement using `motortest` and `mfp0`. Wire EN/DIAG pins and set `MOTOR_EN_DIAG_WIRED 1` in `Movement.cpp` if hardware fault diagnostics are desired.
4. **Wheel Direction Verification**: In `Movement.cpp`, Motor 1 corresponds to the left wheel. Verify on the bench that forward/reverse commands match physical wheel movement using `motortest`; swap INA/INB wiring if inverted.
5. **Photoresistor Calibration**: Calibrate `light_threshold` on analog pin A1 (default **500**, tuned for laboratory lighting). Adjust on the bench using `light <adc>`.
6. **Soil Moisture Calibration**: Determine wet and dry ADC thresholds for the fork sensor on pin A0 and update configuration parameters.

### B. Hardware Component Decisions

7. **Battery Voltage Divider**: Not currently mounted. When installed, set
   `BATTERY_MONITORING_ENABLED 1` in `Sensors.h`, confirm analog pin assignment (`A2` by default), and verify R1/R2 resistor values.
8. **BME680 Sensor**: Code remains in firmware but is omitted from the CSV wiring map. Clarify whether this sensor remains in the bill of materials or should be removed.

### C. Vision Pipeline and Cloud

9. **On-Edge C++ Vision Algorithm**: HSV thresholds and circular ROI analysis are implemented in `VisionBotanist.cpp` on the ESP32-CAM. Offline Python scripts in `vision/` are maintained for experimental parameter calibration.
10. **Cloud Function Stub**: The `upload-image` function in `infra/cloud-functions/upload-image/` is available as a Node 20 stub. Note that current ESP32-CAM firmware uploads directly to Firebase Storage and Firestore via C++ SDKs.

### D. Mobile Application

11. **Android App Integration**: Update mobile app telemetry data models to reflect the 6-sensor distance schema, soil moisture, and water level fields. Add subscriptions to the new `smartvase/{id}/command/ack` topic and `smartvase/{id}/status` LWT topic.

### E. Housekeeping

12. **Mega Debug CLI**: Implemented in `Cli.{h,cpp}`. Available over USB at 115200 baud with diagnostic commands including `i2cscan`, `stats reset`, and `mfp0`.

---

## 11. Conventions for Project Contributors

- **Language**: Project documentation, code comments, identifiers, and logs must be written in **English** (as required by the academic course). Team discussions and chat notes may be conducted in Italian.
- **Legacy Sources**: Files prefixed with `OLD_*` are preserved for historical reference only and must **never** be used as a base for active edits.
- **Pin Mapping**: The CSV file `docs/PINS - Sheet1.csv` is the **single source of truth**
  for hardware wiring. Any `#define` or constant in code that contradicts it is considered a bug.
- **Protobuf**: Do not manually edit generated `.pb.{c,h}` files. All schema modifications must originate in `.proto` files.
- **Memory Allocation**: Do not use Arduino `String` objects on the Mega due to limited SRAM. Use fixed-size character arrays with `strncpy` and explicit null-termination.
- **Non-Blocking Execution**: Never use `delay()` in the Mega main loop. Timing must be managed via `millis()` and state machines.
- **EEPROM Wear Leveling**: Respect write throttling intervals (`EEPROM_*_WRITE_INTERVAL`) to prevent premature wear of EEPROM memory cells.
