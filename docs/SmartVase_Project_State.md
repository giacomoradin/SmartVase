# SmartVase — Firmware status (Mega + ESP32 Hub + ESP32-CAM)

> **Scope of this document.** Operational summary of the state of the
> **three firmware** targets of the project (Arduino Mega, ESP32 Hub,
> ESP32-CAM) and a working list of the TODOs I see open. It is a
> *companion* to [`docs/ARCHITECTURE.md`](ARCHITECTURE.md), which remains
> the complete architecture reference (cloud, vision, app included). Here
> cloud / Python vision / Android app are not discussed unless strictly
> necessary.
>
> Updated as of **2026-07-02** (lab bring-up in progress).

---

## 0-ter. 2026-07-02 update — autonomous plant care (Mega v5.3)

The Mega gains the behavioral layer the project was missing: instead of only
*executing* modes commanded from the app, it can now *decide* them from the
plant's needs. Design document (product thesis, layered model, decision
table): **`docs/Plant_Care_Design.md`**. Offline build SUCCESS
(RAM 66.7% / Flash 25.2%); host test suite all green (7 test binaries,
including the new `test_care_policy`).

- **`CarePolicy.h` (new, pure)** — plant profiles (shade/medium/sun presets),
  daily **light budget** (LDR ADC normalized against the auto-calibrated room
  maximum, integrated into "full-light minutes" — a relative DLI proxy),
  light-scan sector selection, dose/soak/verify watering decision, and the
  `careStep()` daily state machine
  (`NIGHT → SEEK_SUN → BASK → SEEK_SHADE/SHELTER → TOP_UP`). 50 host checks in
  `tests/host/test_care_policy.cpp`.
- **`Care.{h,cpp}` (new module)** — runs the policy at 1 Hz: KPI counters
  (budget %, doses, relocations, UVA minutes), watering cycle over `Pump`
  (doses well under the caps, soak wait between doses, daily dose cap as a
  broken-probe fail-safe, tank guard), **manual-override suspension** (a
  CLI/Hub `setMode` pauses autonomy for 30 min — the operator always wins),
  `care_day_end` daily KPI log toward the Hub. **Disabled by default**
  (`care on` to enable); requires a valid clock (RTC or software fallback).
- **`Movement`** — new **rotating light scan** (`startLightScan`,
  `M_SCAN_ROTATE`/`M_SCAN_ALIGN` internal states, reported as `MOVING` in
  telemetry): ~one in-place turn sampling the LDR over 12 time sectors, then
  align to the best sector and resume the gradient climb. `LIGHT_SCAN_TOTAL_MS`
  (6 s) is time-based and **must be bench-tuned** to a real 360°. Fix: target
  mode back to `IDLE` now stops `M_MOVING` immediately (previously the robot
  kept driving until the 20 s motor safety timeout).
- **`GrowLight`** — new `force()` entry point; with care active the UVA lights
  become the **end-of-day budget top-up** (deficit > 20% within the last hour
  of the daylight window, daily cap), replacing the legacy "IDLE + dark" rule
  (which still applies with care off).
- **`DeviceConfig`** extended with the care fields (enable flag + profile);
  EEPROM layout still fits the 64 B slot (34 B used). ⚠️ The struct change
  invalidates both stored slots via CRC on the first boot of v5.3: config
  falls back to factory defaults → re-run `calib`, `tank`, `light` on the
  bench.
- **CLI** — new `plant [shade|medium|sun]` (profile show/apply) and
  `care [on|off]` (enable + status with daily KPIs); `status`/`config`
  extended.
- **Hardware notes** — RTC CR2032 battery **replaced on 2026-07-01** (not yet
  bench-verified: `rtc` must show a valid time without the software fallback);
  BME680 wiring planned next (then set `BME680_ENABLED 1`).
- **Proto v4.1 — care KPIs in `TelemetryDeep`** (done in a second pass the
  same night): `care_enabled`, `care_state`, `light_budget_pct`,
  `relocations_today`, `water_doses_today`, `growlight_minutes_today`
  appended as tags 22-27 (backward compatible with v4.0 decoders). Regenerated
  offline with the local nanopb venv, canonical files in
  `infra/smartvase-proto/` (the pre-build hook syncs Hub and Mega copies).
  The Mega fills the fields from the Care module in the deep-telemetry
  scheduler; the Hub publishes them as a `care` object in the telemetry JSON
  (see `SmartVase_data_structure.md` §1). Both builds verified: Mega SUCCESS
  (RAM 66.7% / Flash 25.3%), Hub SUCCESS (RAM 15.6% / Flash 80.5%).
- **Whole-day simulation test** (`tests/host/test_care_day_sim.cpp`, second
  pass): drives the pure care layer through simulated days at 1-minute ticks
  and asserts the emergent behavior (sunny-day arc with 2 relocations and a
  100-130% budget; dim-day UVA top-up bounded by window and cap; watering
  cycle convergence and stuck-dry-probe cap). Host suite now at 8 binaries.
- **New CLI `i2cscan`** (bench session 2026-07-02): hardware I²C bus scan with
  hints on the expected devices. First bench run found **0x68 (DS3231) +
  0x57 (AT24C32)** → the HW-084 RTC module is wired and alive; remaining T6
  steps: `rtc set`, 30 s power-off retention test, removal of the module's
  LIR2032 charging resistor (a non-rechargeable CR2032 is fitted).
- ⚠️ **Known regression — CAM no longer builds offline** (verified
  2026-07-02): commit `279da08` added `mobizt/FirebaseClient @ ^2.2.9` to the
  CAM `platformio.ini` and the library is neither in the offline PlatformIO
  cache nor vendored → `build_cam.bat` fails with `HTTPClientError`.
  Remediation (CAM owner): one-time `pio pkg install` from the CAM project
  directory with network access, or vendorize the library like the other
  offline dependencies. Mega and Hub builds are unaffected.

---

## 0-bis. 2026-06-30 update — lab bring-up (Mega v5.2)

Bench session with the robot connected. News on the **Mega** firmware
(offline build SUCCESS, RAM ~61.8% / Flash ~19.5%):

- **UVA grow lights** — new `GrowLight` module on relay D11 (channel 2).
  The lights are wired on the **NC** contact: with the relay at rest they are
  ON (polarity inverted compared to the pump). They turn on automatically
  **only if**: `IDLE` mode **and** `lux < light_threshold` **and** the current
  time is within the daylight window **06:00–20:00**. Pure logic in
  `SensorPolicy.h` (`growLightWanted()` + `withinDaylightWindow()`).
- **Motor driver = Pololu Dual VNH5019 Shield** (model `ash02b`). The
  firmware has been made VNH5019-aware: pins renamed `PWM`/`INA`/`INB` + a new
  **EN/DIAG** line (D41/D39) read as `INPUT_PULLUP` to surface **faults**
  in `diag` (without ever disabling the driver). ⚠️ Symptom found on the
  bench: outputs M1A/M1B/M2A/M2B at 0V with VIN=11.6V/VDD=5.2V → hardware
  cause (suspect #1: **common ground** Mega↔shield, then pin mapping). To be
  closed on the bench.
- **RTC — software fallback clock**: the DS3232 chip does not respond
  (dead CR2032 battery). `setEpoch()` now falls back to a `millis()`-based
  clock; at boot, if no valid time is available, it starts from **08:00**
  (`DEFAULT_BOOT_HOUR`). It is lost on every reset; the real chip takes
  precedence again once it becomes available.
- **`light_threshold` default 600 → 500** (calibrated: dark≈11, lab neon≈540,
  real scale ~0-800). New CLI command **`light <adc>`** to re-tune it on the
  bench.
- **`motor <dir> <ms>`**: the continuous test cap raised from 5 s to **60 s**.
- **MQTT `setMode` command**: confirmed working end-to-end (telemetry showed
  `movement_state: MOVING`); the initial problem was the wrong publish topic
  in the HiveMQ Web Client (`setMode` instead of
  `smartvase/HUB_123456/command/setMode`).

---

## 0. 2026-06-11 update — pre-lab hardening

Repo re-aligned with `origin/main` (the staged renames have been resolved,
`docs/PINS - Sheet1.csv` and this document are committed). All three
PlatformIO builds **compile** locally, offline:

| Target | Result | RAM | Flash |
|--------|-------|-----|-------|
| Mega v5.1   | SUCCESS | 50.1% | 16.4% |
| Hub v1.2    | SUCCESS | 14.3% | 74.2% |
| CAM v2.1    | SUCCESS | 15.9% | 32.9% |

Main changes (details in the sections and commits):

- **Mega v5.1** — pump protection on empty tank/faulty US4 (explicit
  request: `tank_empty_cm` threshold in EEPROM, default 20 cm, tunable from
  the CLI with `tank <cm>`; block on `water`/`pump` and auto-stop during
  irrigation); `standalone` mode for bench tests without the Hub (suspends
  the deadman); local `Ultrasonic` driver (replacing the enjoyneering lib
  which blocked for 50 ms per reading — its `begin()` was, moreover, never
  called: no probe would have read anything) and `RtcDs3232` (the DS3232RTC
  lib is not installable: the machine is offline w.r.t. the registry);
  `rtc set <epoch>` from the CLI; BME680 behind the `BME680_ENABLED 0` flag
  (not mounted, confirmed 2026-06-11); fixed `bme_read_errors` growing on
  every TelemetryDeep even without the sensor; reset of the anti-stuck
  backoff after 60 s of clean driving; glitch-free relay init.
- **Hub v1.2** — **fixed a latent crash**: the global managers were copying
  the FreeRTOS queue handles while they were still NULL (static init); now
  they are created in `setup()` after `xQueueCreate`. **Periodic
  Hub→Mega heartbeat every 30 s** (without it, the Mega's deadman would
  trip even with the Hub connected). Complete serial CLI (`HubCli`):
  Wi-Fi/MQTT provisioning to NVS + passthrough commands to the Mega
  (`water`, `mode`, `soil`, `diag`, `telemetry`, ...) to test the serial
  chain without a network. `WifiManager` no longer clears the credentials
  on timeout and no longer starts the AP automatically: deterministic
  offline boot. MQTT silent if not configured. De-duplicated telemetry
  (publish on TelemetryDeep, timer only as a fallback). `[ACK Mega]` echo
  on the serial port.
- **CAM v2.1** — fixed a **compilation error** (double declaration of
  `t0` in `connectWifi`: v2.0 had never compiled) and a **use-after-free**
  (`fb->len` read after `esp_camera_fb_return`). Non-blocking Wi-Fi with a
  retry every 30 s (previously the loop blocked for 30 s per attempt).
  Serial CLI (NVS provisioning + test `capture` without upload + `stats`).
  Automatic capture only once the full chain is configured; upload metrics
  counted only on real attempts.
- **Build system** — the `build_*.bat` files pointed to a non-existent user
  profile (`C:\Users\Giacomo Radin\...`): they now use `%USERPROFILE%`
  and accept extra arguments (`build_mega.bat -t upload`). Seeded
  PubSubClient in the CAM's libdeps cache (registry unreachable).
- **Docs** — new [`docs/Lab_Bringup_Checklist.md`](Lab_Bringup_Checklist.md):
  step-by-step procedure for tomorrow's bring-up (safe order, troubleshooting
  table, tank calibration). Mega README aligned with v5.1.

Hardware decisions confirmed by Giacomo on 2026-06-11: **only the RTC
DS3232 mounted** (no BME680, no battery divider), relay polarity to be
verified on the bench, tank depth unknown (configurable threshold),
lab **without networking** (debug only via serial CLIs), credential
provisioning via serial CLI.

### Second verification pass (same evening) — 3 more critical bugs

1. **Incompatible Hub↔Mega serial CRC**: the Hub's `SerialManager` used
   CRC-16-IBM (poly 0xA001, LSB-first), the Mega uses CRC16-CCITT (0x1021,
   MSB-first). Every frame was discarded due to a CRC error **in both
   directions**: the serial link would never have worked. Aligned the Hub
   to the Mega's CCITT.
2. **EEPROM/NVS persistence broken from the start**: both `Persistence`
   (Mega) and `ConfigManager` (Hub) computed the CRC *including the CRC
   field itself* (with its old value) while excluding the last 2 data bytes
   → the validation at boot always failed and config/stats/credentials
   reverted to defaults on every power-up. The CRC is now computed over the
   whole struct with the field zeroed out: `tank`, `calib` and the Wi-Fi
   credentials now survive power cycling.
3. **CAM `captureNow` ineffective**: it reset the timer but did not force
   the capture if the uptime was below the interval; it now uses a
   dedicated flag. In addition, the Hub's telemetry fallback timer no
   longer publishes stale data when the Mega is disconnected.

All three builds re-verified SUCCESS after the fixes. Final sweep for
dangerous patterns: no debug print on Serial1 (framing preserved), no
`String` on AVR, `delay` only in the intended spots.

---

## 1. Status of the three firmware versions

| Module                    | Version | Sources                                                  | Build |
|---------------------------|---------|-----------------------------------------------------------|-------|
| Platform Controller Mega  | **v5.1** | `firmware/2_platform-controller_mega/.../src/`          | ✅ SUCCESS (offline) |
| ESP32 Hub                 | **v1.2** | `firmware/1_esp32-hub/.../src/` + `include/`            | ✅ SUCCESS (offline) |
| ESP32-CAM                 | **v2.1** | `firmware/3_esp32-cam/.../src/main.cpp`                 | ✅ SUCCESS (offline) |
| nanopb protocol           | **v4.1** | `infra/smartvase-proto/` (canonical) + synced copies in Hub/Mega | care KPIs appended (tags 22-27), 2026-07-02 |

No firmware has been **flashed** on real hardware yet: the bring-up is
planned for 2026-06-12 following
[`docs/Lab_Bringup_Checklist.md`](Lab_Bringup_Checklist.md).

---

## 2. Arduino Mega — Platform Controller v5.0

**Path**: `firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/`

### 2.1 Modules (one file = one responsibility)

| File                                                                                 | Role                                                                                    |
|--------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------|
| [`src/main.cpp`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/main.cpp) | Setup + non-blocking loop, 4 s WDT, telemetry/heartbeat/log scheduler, health checks |
| [`src/Movement.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Movement.cpp) | Motor state machine + light/shadow seeking + avoidance + STUCK backoff                |
| [`src/Sensors.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Sensors.cpp) | 6 round-robin HC-SR04 with EMA, ADC (lux + soil), BME680, RTC DS3232 + **software fallback clock** |
| [`src/Communication.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Communication.cpp) | Serial framing towards the Hub + stateful RX parser + Command dispatch → side effects |
| [`src/Persistence.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Persistence.cpp) | Dual-slot EEPROM (64 / 128 bytes) + magic + CRC16 + write throttling                  |
| [`src/Pump.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Pump.cpp) | Non-blocking relay D10 active-low, max 60 s duration, updates stats                   |
| [`src/GrowLight.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/GrowLight.cpp) | Relay D11 UVA lights (NC contact); ON only if IDLE + lux<threshold + daylight window 06:00–20:00 |
| [`src/Cli.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Cli.cpp) | Debug CLI over USB (`help`, `status`, `stats`, `sensors`, `diag`, `mode`, `motor`, `motortest`, `pump`, `tank`, `light`, `calib`, `rtc`, `standalone`, `reboot`) |
| [`src/SystemStatus.h`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/SystemStatus.h) | Shared struct: degradedMode, deviceId, reset-request flag, etc.                        |
| [`src/Crc16.{h,cpp}`](../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Crc16.cpp) | CRC16-CCITT (poly 0x1021) shared by Communication and Persistence                      |

### 2.2 PIN map aligned with the CSV (`docs/PINS - Sheet1.csv`)

| Peripheral                | Mega pin                       | Notes                                        |
|----------------------------|---------------------------------|-----------------------------------------------|
| US1 — front-top            | TRIG 33 / ECHO 35              | navigation                                    |
| US2 — front-right          | TRIG 26 / ECHO 27              | navigation                                    |
| US3 — front-left           | TRIG 36 / ECHO 37              | navigation                                    |
| US4 — water tank           | TRIG 50 / ECHO 51              | tank water level                              |
| US5 — left side            | TRIG 4  / ECHO 5               | navigation                                    |
| US6 — right side           | TRIG 28 / ECHO 29              | navigation                                    |
| Soil-moisture fork         | A0                             | raw ADC 0..1023                               |
| Photoresistor              | A1                             | raw ADC 0..1023 (moved from A0); `light_threshold` default **500** |
| Battery (divider)          | A2                             | `BATTERY_MONITORING_ENABLED = 0` by default   |
| Motor L (VNH5019)          | PWM 6, INA 43, INB 45, EN/DIAG 41 | Pololu Dual VNH5019; EN optional (fault read) |
| Motor R (VNH5019)          | PWM 7, INA 47, INB 49, EN/DIAG 39 | ⚠️ common GND Mega↔shield mandatory           |
| Pump relay                 | D10 (IN1) — active LOW         | `Pump` module                                 |
| UVA grow-light relay       | D11 (IN2)                      | `GrowLight` module; lights on **NC** contact (rest = ON) |
| RTC DS3232                 | SDA 20 / SCL 21 (I²C 0x68)     | software fallback clock if chip absent/dead battery |
| BME680                     | I²C 0x76                       | **not present in the CSV** — see §6           |
| UART towards Hub           | Serial1 — D18(TX1) / D19(RX1)  | 115200 baud                                   |
| USB / debug CLI            | Serial — D0(RX) / D1(TX)       | 115200 baud                                   |

### 2.3 Main loop (`main.cpp`)

```
wdt_reset()
cli.tick()                                  # USB CLI
comm.handleSerial()                         # RX framing + Command dispatch
sensors.sampleSensors()                     # round-robin: 1 US probe every 30 ms + ADC
pump.tick()                                 # auto-off on timer
movement.handleMovementSM(obstacleView, lux, config, stats, degraded)
if (now - lastFastTelemetryMs  >= 1000 ms) sendFastTelemetry()
if (now - lastDeepTelemetryMs  >= 30 s)    sendDeepTelemetry()
if (now - lastHeartbeatMs      >= 5 s)     sendHeartbeat()
if (now - lastLogDrainMs       >= 200 ms)  drainLogQueue()
persistence.saveStats(false)                # internally throttled
health checks: SRAM hysteresis 800/1200 + hub deadman 120 s
if (softResetRequested) performSoftReset()  # via WDT_15MS
```

### 2.4 Resilience & observability (key concepts)

- **4 s WDT**: any stall > 4 s causes a reset, counted in `watchdog_resets`.
- **MCUSR mirror** in `.init3`: distinguishes power-on from watchdog reset at boot.
- **SRAM hysteresis**: degraded mode kicks in below 800 B free, exits above
  1200 B — no oscillation around the threshold.
- **Hub deadman**: if the Hub is silent for > 120 s → degraded mode
  (`hub_missing`), motors stopped, pump off.
- **Circular log queue** (20 slots) with `noInterrupts()` only on the producer's
  enqueue. Overflow counted in stats.
- **Dual-slot EEPROM** with magic number + CRC16, slot rotation on every
  write (wear leveling), throttling 60 s (config) / 300 s (stats).
- **Serial framing** SOF=`0xAA`, 2-byte big-endian length, protobuf payload,
  CRC16-CCITT (poly `0x1021`). Errors counted in `pb_decode_failures`.
- **Pump safety**: hard cap of 60 s per cycle, rejected if already active or
  in degraded mode.
- **Motor safety**: 20 s of uninterrupted activity timeout → forced stop.

### 2.5 `Movement` state machine

```
M_IDLE ───────► M_MOVING ───────► M_AVOID_START ───► M_AVOID_REVERSING ───► M_AVOID_TURNING
   ▲                ▲                                                              │
   │                │                                                              ▼
   │                └──────────────────── front clear ◄─────────────────────── M_AVOID_TURNING
   │                                                                              │
   └────────────────── backoff expired ◄──── M_STUCK ◄── 3 failed attempts ──────┘
```

- Front obstacle threshold: 20 cm (US1 ∪ US2 ∪ US3). Side: 12 cm (US5/US6).
- Rotation direction during avoidance: prefers the free side; random if
  both are free or both are blocked.
- Stuck backoff: starts at 30 s, +10 s on every re-entry into `M_STUCK`.

### 2.6 Handled commands (Hub → Mega)

`water` · `setMode {IDLE, LIGHT, SHADOW}` · `stop` · `requestDiagnostics`
(returns a `TelemetryDeep`) · `setMotionParams {reverse_ms, turn_ms}`
(persisted in EEPROM) · `readSoil` (returns the current ADC in the
`value` field of `CommandResponse`) · `softReset`.

---

## 3. ESP32 Hub — Logic & Web Hub v1.0

**Path**: `firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/`

### 3.1 FreeRTOS architecture

3 tasks pinned to the two cores + 4 communication queues.

| Task              | Core | Prio   | Stack | Role                                                        |
|-------------------|------|--------|-------|---------------------------------------------------------------|
| `TaskSerialMega`  | 1    | 3 high | 4 KB  | UART2 ↔ Mega, protobuf encode/decode + CRC16 framing         |
| `TaskMqttLink`    | 0    | 2 med  | 8 KB  | PubSubClient TLS towards HiveMQ Cloud, 5 s reconnect         |
| `TaskMainLogic`   | 0    | 1 low  | 8 KB  | JSON ↔ Protobuf bridge, telemetry timer, deadman switch      |
| `loop()` (arduino)| —    | idle   | —     | placeholder for CLI (today only `vTaskDelay(100)`)           |

Queues (`xQueueCreate`, capacity 10):
- `serialRxQueue` Mega → MainLogic — `SerialMessage { WrapperMessage }`
- `serialTxQueue` MainLogic → Mega — same, outbound
- `mqttTxQueue`   MainLogic → Mqtt  — `MqttMessage { topic[64], payload[512] }`
- `mqttRxQueue`   Mqtt → MainLogic  — `MqttCommand { ts, topic[64], payload[512] }`

### 3.2 Modules

| File                                                                                                                                                | Role                                                                                                               |
|-----------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| [`src/main.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/main.cpp)                                            | NVS + Wi-Fi + queue + task bootstrap                                                                               |
| [`src/ConfigManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/ConfigManager.cpp)                          | Preferences/NVS wrapper for `DeviceConfig` (Wi-Fi + MQTT + webhook), magic + CRC16                                 |
| [`src/WifiManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/WifiManager.cpp)                              | STA with timeout; **provisioning AP** fallback on port 80 (`ESPAsyncWebServer`) if the SSID is empty or the connection fails |
| [`src/SerialManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/SerialManager.cpp)                          | Serial2 (`MEGA_RX_PIN=16, MEGA_TX_PIN=17`) ↔ Mega: framing parser + WrapperMessage encoding                       |
| [`src/MQTTManager.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/MQTTManager.cpp)                              | `WiFiClientSecure + PubSubClient`, HiveMQ CA in `include/hivemq_ca_cert.h`, LWT `offline` on `/status`, retained    |
| [`src/MainLogic.cpp`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/MainLogic.cpp)                                  | Core: last fast/deep telemetry cache, 60 s publish timer, 130 s Mega deadman, JSON ⇄ Protobuf, command acks        |
| [`include/hivemq_ca_cert.h`](../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/include/hivemq_ca_cert.h)                    | CA cert for HiveMQ Cloud (shared with the CAM via `infra/hivemq_ca_cert.h`)                                        |

### 3.3 Published / subscribed MQTT topics

`{id} = HUB_<last 3 MAC bytes>` (e.g. `HUB_A1B2C3`).

| Topic                              | Direction         | Payload                                                                    |
|-------------------------------------|--------------------|------------------------------------------------------------------------------|
| `smartvase/{id}/telemetry`         | Hub → Cloud       | JSON with `distances_cm{top,front_right,front_left,left,right}` + ambient + counters |
| `smartvase/{id}/logs`              | Hub → Cloud       | JSON `{timestamp_ms, level, event, detail, source_device}`                 |
| `smartvase/{id}/alarm`             | Hub → Cloud       | JSON `{type, detail}` (e.g. `mega_offline`, `tx_queue_full`)                |
| `smartvase/{id}/status`            | Hub → Cloud       | `online` / `offline` (LWT, retained)                                       |
| `smartvase/{id}/command/#`         | Cloud → Hub       | JSON `{type, cmd_id, …}` — mapping in `processMqttCommand`                 |
| `smartvase/{id}/command/ack`       | Hub → Cloud       | JSON `{cmd_id, status, detail, value, exec_time_ms}`                       |

Command types mapped Hub → Mega: `setMode | water | stop | requestDiagnostics |
setMotionParams | readSoil | softReset`. Unrecognized commands → `alarm`
`unknown_command_type`.

### 3.4 Telemetry flow

1. The Mega sends `TelemetryFast` every 1 s and `TelemetryDeep` every 30 s on Serial1.
2. The Hub updates `_lastFastTelemetry` and `_lastDeepTelemetry` in `processSerialMessage`.
3. MQTT publish:
   - **Immediate** upon arrival of `TelemetryDeep` (fast + deep composite);
   - **Periodic** every 60 s via `telemetryTimerCallback` (same composite).
4. `Heartbeat` only updates `_lastMegaHeartbeatMs` (no publish).
5. If the Mega is silent for > 130 s → publish `alarm` with `type=mega_offline`. When
   it comes back: `alarm` with `type=mega_online`. 130 s = 120 s Mega deadman + 10 s
   network/serial margin.

---

## 4. ESP32-CAM — Vision Co-Processor v2.0

**Path**: `firmware/3_esp32-cam/Radin_Giacomo_SmartVase_VisionCoProcessor_ESP32CAM/`

Full refactor from bench-code (v14, dumped frames to Serial) to
autonomous Wi-Fi/MQTT/Cloud firmware.

### 4.1 Life cycle

```
setup():
   disable brown-out
   loadConfig() / loadStats() from NVS
   makeDeviceId() = "CAM_" + last 3 MAC bytes
   initCamera() OV2640: SVGA (with PSRAM) / VGA (without), JPEG quality 12
   connectWifi() + NTP (pool.ntp.org, max 3 s wait)
   mqttInit() + mqttReconnect()

loop():
   if WiFi is down → reconnect
   if MQTT is down → reconnect; mqttClient.loop()
   every cfg.interval_s (default 300 s):
      esp_camera_fb_get()
      crc32_le(buffer)
      uploadJpeg() → streaming multipart POST to cfg.upload_url
                     (the Cloud Function replies with {image_url})
      esp_camera_fb_return()
      publishVisionImage(url, size, crc, capture_time)
      saveStats() to NVS
```

### 4.2 Config (NVS namespace `cam`)

| Key           | Type   | Default                                    | Notes                                  |
|----------------|--------|----------------------------------------------|-----------------------------------------|
| `wifi_ssid`   | string | (empty)                                    | must be written by hand the first time  |
| `wifi_pass`   | string | (empty)                                    |                                         |
| `mqtt_broker` | string | (empty)                                    | `<id>.s1.eu.hivemq.cloud`               |
| `mqtt_port`   | uint16 | 8883                                       | TLS                                     |
| `mqtt_user`   | string | (empty)                                    |                                         |
| `mqtt_pass`   | string | (empty)                                    |                                         |
| `upload_url`  | string | (empty)                                    | `upload-image` Cloud Function endpoint  |
| `interval_s`  | uint32 | 300                                        | seconds between captures                |

Persistent stats (namespace `cam_stats`): `succ_frames`, `fail_frames`,
`upload_err`, `mqtt_err`, `total_cap_ms`.

### 4.3 MQTT topics

`{id} = CAM_<last 3 MAC bytes>`.

| Topic                              | Direction   | Payload                                                    |
|--------------------------------------|-------------|--------------------------------------------------------------|
| `smartvase/{id}/vision/image`      | CAM → Cloud | JSON `{timestamp_utc, device_id, image_url, size_bytes, crc32, capture_time_ms, content_type}` |
| `smartvase/{id}/vision/status`     | CAM → Cloud | `online` / `offline` (LWT, retained)                       |
| `smartvase/{id}/vision/command/#`  | Cloud → CAM | JSON `{type}` — supported: `captureNow`, `reboot`           |

### 4.4 Notable implementation details

- **Streaming upload**: the multipart body is never allocated on the heap. An
  internal `MultipartStream` class concatenates header + JPEG buffer + footer
  on-the-fly while `HTTPClient::sendRequest("POST", Stream*, total)` reads it
  in chunks. Useful given the ESP32-CAM's limited heap and typical SVGA
  sizes of 20–80 KB.
- **TLS towards the Cloud Function**: currently `client.setInsecure()`. Marked
  TODO in the code — see §6.
- **TLS towards HiveMQ**: hardcoded CA cert (`infra/hivemq_ca_cert.h`), validated.
- **Best-effort NTP**: 3 s timeout in `connectWifi`. If NTP fails, the
  `timestamp_utc` ends up 0/garbage.

---

## 5. Hub ↔ Mega serial protocol (`smartvase.proto` v4.1)

Canonical file: [`infra/smartvase-proto/smartvase.proto`](../infra/smartvase-proto/smartvase.proto)
(identical copy also in `firmware/.../src/smartvase.proto` and
`firmware/.../include/smartvase.proto`).

### 5.1 Messages

- **`TelemetryFast`** — 5 navigation distances (`top, front_right, front_left,
  left, right`) + `water_level_cm` + `soil_moisture` (ADC) + `lux` (ADC) +
  `movement_state` + `epoch_s` (RTC) + `device_id`.
- **`TelemetryDeep`** — BME680 (`temperature_c, humidity_percent, pressure_hpa,
  gas_resistance_ohms`) + `uptime_s, free_ram_bytes, epoch_s` + 9 cumulative
  counters + `battery_voltage` (reported as 0 if monitoring is off) + `device_id`.
- **`Log`** — `LogLevel ∈ {INFO, WARN, ERROR, CRITICAL}`, `event[24]`,
  `detail[32]`, `timestamp_ms`, `source_device[16]`.
- **`Heartbeat`** — `uptime_s, is_degraded, device_id`.
- **`Command`** (oneof): `water | set_mode | stop | request_diagnostics |
  set_motion_params | read_soil | soft_reset`, `cmd_id = 99`.
- **`CommandResponse`** — `status ∈ {OK, ERROR}`, `detail[64]`, **`value` (int32)**
  for responses with a numeric payload (e.g. `readSoil` returns the ADC value),
  `cmd_id`, `exec_time_ms`.

### 5.2 Serial framing

```
0xAA | len_hi | len_lo | payload (protobuf, ≤256 B) | crc16_hi | crc16_lo
```

CRC16-CCITT, poly `0x1021`, init `0x0000`. 6-state RX parser.
Errors → `stats.pb_decode_failures++` and the frame is dropped.

### 5.3 nanopb codegen

Script `infra/smartvase-proto/generate_proto.bat`. Arduino IDE constraint:
in the generated `.pb.h`, `#include <pb.h>` must be replaced with
`#include "pb.h"`.

---

## 6. Build & dependencies

PlatformIO wrappers at the repo root: `build_mega.bat`, `build_hub.bat`,
`build_cam.bat`. They use the `pio.exe` in `C:\Users\Giacomo Radin\.platformio\penv\Scripts\`.

### 6.1 `lib_deps`

| Env                | Libraries                                                                                                  |
|--------------------|-----------------------------------------------------------------------------------------------------------|
| `megaatmega2560`   | `adafruit/Adafruit BME680 Library@^2.0.1`, `paulstoffregen/Time@^1.6.1` (HC-SR04 and DS3232 are **local drivers** in `src/`, see `Ultrasonic`/`RtcDs3232`; BME680 only with `BME680_ENABLED=1`) |
| `esp32dev` (Hub)   | `ESP32Async/AsyncTCP@^3.3.2`, `ESP32Async/ESPAsyncWebServer@^3.6.0`, `bblanchon/ArduinoJson@^6.19.4`, `knolleary/PubSubClient@^2.8` |
| `esp32cam`         | `bblanchon/ArduinoJson@^6.19.4`, `knolleary/PubSubClient@^2.8` (+ `BOARD_HAS_PSRAM`, `-mfix-esp32-psram-cache-issue`) |

Notes:
- The Mega **no longer has** any dependency on an external EEPROM/WString
  library (it is part of the Arduino core).
- The Hub **declares** AsyncTCP + ESPAsyncWebServer (used by `WifiManager`
  for the provisioning AP). There is not yet a real user web UI.
- The CAM does not declare `esp_camera` because it is part of the
  Arduino-ESP32 core (board `esp32cam`).

### 6.2 Discrepancies with old documentation

[`firmware/2_platform-controller_mega/README_PlatformController_Arduino.md`](../firmware/2_platform-controller_mega/README_PlatformController_Arduino.md)
is still **pre-refactor** (talks about v3, `smartvase_v3.proto`, "HCSR04 by
gamegine" instead of `enjoyneering/HCSR04`, is missing DS3232RTC, is missing the
extended CLI). To be realigned to v5 or removed in favor of ARCHITECTURE.md.

---

## 7. What's missing or needs fixing — firmware TODO

> **Note 2026-06-11**: many items in this section were closed with the
> pre-bring-up hardening (see §0): builds of the 3 targets ✅, BME680
> clarified (absent, behind a flag) ✅, battery flag-off confirmed ✅,
> Hub+CAM CLI provisioning ✅, CAM `t0` bug ✅, fb use-after-free ✅,
> non-blocking CAM Wi-Fi ✅, upload metrics ✅, duplicated Hub telemetry
> publish ✅, anti-stuck backoff with reset ✅, empty-tank pump
> protection ✅, bench deadman (standalone) ✅, Hub→Mega heartbeat ✅.
> Still open: flashing/validation on HW (tomorrow), CAM upload TLS pinning,
> vision/result in the Hub, autonomous automation, OTA, command rate-limit,
> consolidating proto/cert copies.

Tags: **[BLK]** = blocking for bringing the robot online · **[FUNC]** = feature
to complete · **[POL]** = polish / quality / security.

### 7.1 Bring-up on the new prototype

- **[BLK]** Build & flash the three firmwares on the real HW.
  Never done after the v5 refactor: expect 1–2 rounds of lib_deps /
  include path / HCSR04 symbol-name fixes depending on the installed version.
- **[BLK]** Validate the PIN map on the bench: common GND Mega↔shield VNH5019
  (cause #1 of 0V outputs) and which PWM/INA group corresponds to the physically
  "left" motor (see the note in §4.2 of ARCHITECTURE.md).
- **[BLK]** Wire and configure the CAM via NVS (`wifi_ssid`, `wifi_pass`,
  `mqtt_*`, `upload_url`): without these keys the CAM does not start. **A
  provisioning mechanism is missing** (today they must be written from the host
  via a Preferences tool or a throwaway sketch).
- **[BLK]** Verify the physical presence of the BME680: it is used by the
  firmware via I²C 0x76 but **is not listed in the CSV** of the pins. If it is
  no longer in the BOM, it must be removed from the firmware (or stubbed with
  a permanent `bme_status=false`).

### 7.2 Mega — known gaps

- **[FUNC]** Battery: `BATTERY_MONITORING_ENABLED=0` ⇒ the `battery_voltage`
  field in `TelemetryDeep` is always 0. When the divider is wired
  (R1=30k, R2=7.5k on A2), just flip the flag to 1; the ratio may need
  re-calibration and/or the pin may need to move if A2 is reassigned.
- **[FUNC]** The `light_seeking_sessions` and `shadow_seeking_sessions`
  statistics are incremented in `Movement` but **not included** in `TelemetryDeep`
  nor printed by the CLI `stats`. Add or remove them.
- **[FUNC]** Same for `escape_attempts`: counted but never published.
- **[POL]** `Movement::handleMovementSM`: the stuck backoff grows by +10 s
  per event and **never resets** (even after a long period of clean
  navigation). After many stuck events the robot sleeps for minutes.
  Suggestion: halve/reset after N seconds of `M_MOVING` without events.
- **[POL]** Light/shadow seeking is a continuous "binary" turn until the
  light crosses the threshold. Missing:
  - a timeout / maximum number of turns before switching to `IDLE`;
  - a measurement of the light *gradient* (today it always turns in the
    same direction, it can spin in circles).
- **[POL]** `ReadSoilCommand` returns **a single** ADC reading, with no
  filtering or conversion to calibrated units. For app usage it probably
  already needs the EMA value / a mapped range.
- **[POL]** `Communication::executeCommand`: no rate-limit. A malicious
  client can saturate the pump with many consecutive `water` commands within
  60 s (even though `start()` rejects if already active, that only covers
  concurrency).
- **[POL]** Persistence: the two config slots are 64 bytes apart and the stats
  256 bytes; the current `sizeof(DeviceConfig)` is ~14 bytes and
  `CumulativeStats` ~60 bytes. Wide margin, but watch for overlap if fields
  are added later.
- **[POL]** Default `light_threshold` in `Persistence.cpp`: needs to be
  revisited and verified against the real photoresistor (today it is initialized
  in `Movement`'s constructor to 600 and then overwritten by the config: redundant).

### 7.3 Hub — known gaps

- **[BLK]** **No subscription to `smartvase/{id}/vision/result`** —
  the vision pipeline results never make it back to the Hub and therefore
  can never influence the robot's behavior. The architecture document
  expects this closed loop, the code does not implement it.
- **[FUNC]** No **autonomous automation** (no `applyDefaultPlantLogic`):
  the robot only waters if the app sends a `water`, and there is no
  automatic choice of `LIGHT`/`SHADOW` based on sensors. Design decision
  to be revisited — see the questions at the end.
- **[FUNC]** The Arduino task's `loop()` is effectively empty (`handleCLI()`
  is a comment). No debug CLI on the Hub over USB, while the Mega has one.
- **[FUNC]** `processMqttCommand`: does not validate that the actual command
  topic is consistent with the `type` in the JSON. A `{"type":"water"}`
  payload published on `smartvase/.../command/setMode` is still executed.
- **[POL]** Telemetry publish: happens both when `TelemetryDeep` arrives
  (every 30 s) and from the timer (every 60 s). In practice, two nearly
  identical publishes within 30 s. Either remove the timer or detach the
  publish from `deep`.
- **[POL]** The distances (which change much faster than the other metrics)
  are only published at the deep/timer cadence. Real-time app behavior
  needs a dedicated higher-frequency topic (e.g. `telemetry/fast` every
  1–2 s with distances + state only).
- **[POL]** `MQTTManager::reconnect`: fixed 5 s backoff, no jitter. Under a
  network blackout it polls regularly and predictably — acceptable, worth
  keeping an eye on.
- **[POL]** `WifiManager` provisioning AP: present but missing a default
  HTML page being served; check what `ESPAsyncWebServer` returns. I have not
  read the full file.

### 7.4 ESP32-CAM — known gaps

- **[BLK]** NVS provisioning: no tool to write `wifi_*`,
  `mqtt_*`, `upload_url` the first time. Either (a) a throwaway sketch that
  does `prefs.putString(...)`, or (b) a provisioning AP like the Hub's, or
  (c) a temporary hard-code in `setup()` during bring-up are needed.
- **[BLK]** `uploadJpeg()` uses `WiFiClientSecure::setInsecure()` towards
  the Cloud Function: TLS is not validated. The endpoint's cert/CA must be
  pinned before taking the robot outside the house (decision shared with Fia).
- **[FUNC]** The only supported MQTT commands are `captureNow` and `reboot`.
  Missing: `setInterval(s)`, `setQuality(0..63)`, `getStats`, `flashOn/Off`.
- **[FUNC]** Hub ↔ CAM coordination: the Hub never requests a capture; the
  CAM only captures on the interval timer. Interesting conditions
  ("just finished moving", "stable light") need a command from the Hub.
- **[POL]** `loop()` calls `delay(50)` instead of handling timers more
  reactively, and `mqttClient.loop()` can block for a few ms — acceptable.
- **[POL]** If `cfg.upload_url` is empty, every cycle counts an `upload_err`
  but `successful_frames` is still incremented (the capture succeeded).
  The metrics are misleading.
- **[POL]** Double `unsigned long t0` in `connectWifi()`: there is a
  shadowing bug (the second declaration comes after the first `while`).
  Verify whether it compiles or whether it already needs cleaning up.

### 7.5 Cross-cutting across the three firmwares

- **[FUNC]** **OTA absent** everywhere. Critical if the robot ends up in
  areas where flashing over USB is inconvenient.
- **[FUNC]** No runtime versioning: each firmware has its "v5.0/v1.0/v2.0"
  in a comment but does not publish it in telemetry/logs. Add `fw_version`
  to `TelemetryDeep` and to the `vision/image` payload.
- **[POL]** The proto has 3 on-disk copies (Mega, Hub, infra). Today
  `generate_proto.bat` updates `infra/smartvase-proto/` and *probably*
  needs to be manually copied to the other two. Verify and centralize
  (single source + simlink/copy step in the `.bat`).
- **[POL]** The Hub and CAM share `infra/hivemq_ca_cert.h` ✅. But there are
  also local copies: `firmware/1_esp32-hub/.../include/hivemq_ca_cert.h`
  and `firmware/3_esp32-cam/.../src/hivemq_ca_cert.h`. Should be consolidated
  with a PlatformIO `extra_scripts` that copies them at build time, or an
  include path relative to `infra/`.

---

## 8. Areas not covered by this document

To avoid duplicating ARCHITECTURE.md:

- **Cloud pipeline** (HiveMQ ⇄ Cloud Functions ⇄ Firestore ⇄ App/Vision):
  see `docs/SmartVase_data_structure.md` and `infra/cloud-functions/`.
- **Python Vision** (`vision/`): pipeline + quality_gate + leaf_health already
  with 18 passing pytest tests. Not covered by this doc.
- **Android App** (Francesco): outside this repo.
- **Server scripts** (`server/mqtt_listener.py`): dev bridge.

---

## 9. How to get your bearings if you come back to the code tomorrow

1. **Open ARCHITECTURE.md** for the overall picture.
2. **Open this file** for the firmware-only status and TODOs.
3. **PIN map**: `docs/PINS - Sheet1.csv` is the source of truth for wiring.
4. **Build**: from the repo root, `build_mega.bat` / `build_hub.bat` /
   `build_cam.bat`.
5. **Mega debug**: USB Serial at 115200 + the `help` command.
6. **Hub debug**: USB Serial at 115200 (ESP_LOG logs with `CORE_DEBUG_LEVEL=4`).
7. **CAM debug**: USB Serial at 115200; for the MQTT topics subscribe to
   `smartvase/CAM_+/#` on a client (e.g. MQTT Explorer pointed at HiveMQ).

---

## 10. Aligning the working tree

To do before any firmware change:

```powershell
git status                              # check staged renames
git fetch origin
git pull --ff-only origin main          # local main is 2 commits behind
git log --oneline -5                    # should show 83e8f4c on top
```

If `--ff-only` fails because of the staged renames, evaluate whether those
renames are already part of `origin/main` (they are, see §0); in that case
`git reset --hard origin/main` after checking that no uncommitted local
work is lost (today there is nothing new in the working tree apart from the
already-duplicated renames).

---

_Status of this document: first draft, 2026-05-27._
