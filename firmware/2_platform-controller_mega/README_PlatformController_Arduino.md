# SmartVase - Platform Controller Firmware (Arduino Mega)

Firmware for the SmartVase **Platform Controller**. Version **5.4.0** (working tree):
on top of the v5.1 hardening (pump tank protection, extended CLI, standalone
mode, local HC-SR04/DS3232 drivers) and the v5.2 batch (irrigation
rate-limiting, EEPROM no-op on `setMotionParams`, EMA on lux/soil,
anti-circling, seeking/escape counters in `TelemetryDeep`, `GrowLight` module,
VNH5019-aware motor driver, software fallback RTC clock, `light <adc>` CLI),
**v5.4.0 adds the autonomous plant-care layer** (`Care` + `CarePolicy.h`,
design in `docs/Plant_Care_Design.md`): daily **light budget** (relative DLI
proxy with LDR auto-calibration), rotating **light scan** ("solar compass"
with the single fixed LDR), **dose/soak/verify** autonomous watering,
per-plant **profiles** (shade/medium/sun presets in EEPROM), UVA grow lights
repurposed as the end-of-day budget **top-up**, and daily KPIs
(`care` CLI command + `care_day_end` log toward the Hub). Disabled by
default: it is enabled explicitly with `care on`.
**Authoritative** architectural reference: `docs/ARCHITECTURE.md`.
PIN map: `docs/PINS - Sheet1.csv`.

## Architecture

The Mega is "the arm": it directly drives the hardware (motors, pump,
sensors, RTC) and talks **only** to the ESP32 Hub over Serial1 at 115200 baud
(Protobuf+CRC16 framing).

Modules (`src/`):

| File              | Responsibility                                                                            |
|-------------------|---------------------------------------------------------------------------------------------|
| `main.cpp`        | Setup + non-blocking loop, telemetry/heartbeat/log scheduler, WDT, degraded mode           |
| `Sensors.{h,cpp}` | 6 HC-SR04 (round-robin), RTC DS3232, humidity fork, photoresistor, BME680 (flag)           |
| `Movement.{h,cpp}`| Motor state machine (IDLE/MOVING/AVOID*/STUCK/SCAN*); proportional differential steering + wall-following (`driveMotors`), light-seek / shadow-seek, rotating **light scan** |
| `NavPolicy.h`     | Pure navigation logic (no HW): proportional obstacle avoidance + wall-following, host-testable |
| `Care.{h,cpp}`    | Autonomous plant-care layer (L2): light-budget accounting, care state machine, dose/soak/verify watering, manual-override suspension |
| `CarePolicy.h`    | Pure care logic (no HW): plant profiles/presets, daily light budget, scan sector selection, care decision table, host-testable |
| `Pump.{h,cpp}`    | Non-blocking irrigation pump (relay D10, 60s max safety)                                    |
| `GrowLight.{h,cpp}` | UVA lights on relay D11 (NC contact). Care active: end-of-day budget top-up; otherwise legacy rule (IDLE + lux<threshold + daylight window 06:00 to 20:00) |
| `SensorPolicy.h` / `CommandPolicy.h` | Pure functions (no HW) for tank/seeking/lights and command clamp/rate-limit, unit-testable on host |
| `Persistence.{h,cpp}` | Dual-slot EEPROM with magic+CRC16, wear leveling                                        |
| `Communication.{h,cpp}` | SOF/len/payload/CRC16 serial framing, state parser, log queue, command dispatcher  |
| `Cli.{h,cpp}`     | Debug CLI over Serial USB (threshold provisioning, motor/pump tests, standalone)            |
| `Ultrasonic.{h,cpp}` | Minimal local HC-SR04 driver (pulseIn with timeout, no fixed delay)                       |
| `RtcDs3232.{h,cpp}`  | Minimal local DS3232 driver (get/set epoch + OSF flag via Wire)                          |
| `SystemStatus.h`  | Shared status struct (degraded mode, standalone, deviceId, fw version)                      |
| `smartvase_aliases.h` | Typedefs/defines for the nanopb symbols + internal C++ types                            |

## Authoritative PIN map

See `docs/PINS - Sheet1.csv`. Summary:

| Peripheral            | Pin                              |
|------------------------|-----------------------------------|
| US1 (front-top)        | trig D33 / echo D35              |
| US2 (front-right)      | trig D26 / echo D27              |
| US3 (front-left)       | trig D36 / echo D37              |
| US4 (water tank)       | trig D50 / echo D51              |
| US5 (left)             | trig D4  / echo D5               |
| US6 (right)            | trig D28 / echo D29              |
| Motor L (VNH5019)      | PWM=D7, INA=D41, INB=D43, EN/DIAG=not wired |
| Motor R (VNH5019)      | PWM=D6, INA=D45, INB=D47, EN/DIAG=not wired |
| Pump relay             | IN1=D10 (active-low)             |
| UVA grow-light relay   | IN2=D11 (NC contact: rest=on)    |
| RTC DS3232 (I²C)       | SDA=D20, SCL=D21 (addr 0x68); software fallback clock |
| Moisture fork          | A0                               |
| Photoresistor (LDR)    | A1 (moved from A0)               |
| Battery (divider)      | A2 (disabled until wired)        |
| BME680 (I²C)           | addr 0x76                        |

The pin constants are centralized in `Sensors.cpp` and `Movement.cpp`.

## Dependencies (PlatformIO)

```ini
lib_deps =
    adafruit/Adafruit BME680 Library @ ^2.0.1
    paulstoffregen/Time @ ^1.6.1
```

HC-SR04 and DS3232 are handled by local drivers in `src/` (`Ultrasonic`,
`RtcDs3232`): no registry download, the builds work offline.
The Nanopb files (`pb_*.c/h`, `smartvase.pb.{c,h}`) are already in `src/`
and are compiled with the sketch.

## Build

```
build_mega.bat
```

Equivalent to `pio run -d firmware/2_platform-controller_mega/...`.

## Key features

- **Non-blocking**: no `delay()` in the main loop (except for
  `Movement::testMove`, used only by the manual CLI).
- **Hardware watchdog**: `WDTO_4S`. Reset count saved in the EEPROM stats.
- **Degraded mode**: activates if `freeRam() < 800 B` or if the Hub is
  silent for >120 s. Stops the motors, stops the pump, ignores movement commands.
- **Dual-slot EEPROM**: alternating double slot with magic number + CRC16
  for `DeviceConfig` (60 s throttle) and `CumulativeStats` (300 s throttle).
- **Serial framing**: `SOF=0xAA | len(2) | payload | crc16(2)`, CRC-CCITT
  (poly `0x1021`).
- **Log queue**: circular, 20 slots, drained every 200 ms by the main loop.
- **Tank protection (v5.1)**: the pump does not start (and stops on its own)
  if US4 measures a distance beyond `tank_empty_cm` (default 20, tunable with
  `tank <cm>` from the CLI) **or** if the reading is invalid (fail-safe
  against dry running). Applies to the remote `water` command and the CLI `pump`.
- **Standalone mode (v5.1)**: `standalone on` from the CLI suspends the Hub's
  deadman for bench tests without the ESP32 connected.
- **Autonomous care (v5.4.0)**: with `care on`, the robot manages the plant's
  day on its own: seeks the brightest reachable spot in the morning (rotating
  light scan + gradient climb), basks accumulating the daily light budget,
  seeks shade when the budget is met (or on the over-light heat proxy),
  waters in dose/soak/verify cycles on the soil hysteresis band, and tops up
  a light deficit with the UVA lights at the end of the day (daily cap).
  Requires a valid clock (`rtc`); a manual `mode`/`setMode` suspends it for
  30 minutes (operator priority). All L0 safeties (degraded mode, tank
  guard, pump caps, motor timeouts) remain in charge below it. Design and
  decision table: `docs/Plant_Care_Design.md`.
- **Supported commands** (from the Hub):
  `WaterCommand`, `SetModeCommand`, `StopCommand`,
  `RequestDiagnosticsCommand`, `SetMotionParamsCommand`,
  `ReadSoilCommand`, `SoftResetCommand`. Every command produces a
  `CommandResponse` (status OK/ERROR, detail, value, cmd_id, exec_time_ms).

## Debug CLI (Serial USB, 115200, newline)

`help` shows the full menu: `status`, `stats`, `stats reset`, `config`, `sensors`, `diag`,
`i2cscan`, `tank [cm]`, `light <adc>`, `rtc [set <epoch>]`, `mode <idle|light|shadow>`,
`plant [shade|medium|sun]`, `care [on|off]`, `wall <left|right|off>`,
`motor <f|b|l|r> <ms>`, `mfp0`, `motortest`, `calib <l> <r>`, `pump <ms>`,
`standalone <on|off>`, `version`, `reboot`.
The full test procedure is in `docs/Lab_Bringup_Checklist.md`.

## Open TODOs

- [ ] Bench-validate the care layer end-to-end (see `docs/Plant_Care_Design.md`
      §8, horizon H1): light scan rotation time (`LIGHT_SCAN_TOTAL_MS` in
      `Movement.cpp`, time-based, measure a real 360°), soil/wet thresholds
      per plant, dose sizing.
- [ ] RTC hardware replacement: The HW-084 module (DS3231) was found faulty on the bench (silent on I2C). Until replaced, the Mega uses a resilient `millis()` software fallback clock that is automatically synchronized by every Hub heartbeat carrying the Hub NTP epoch (proto v4.2). Once replaced, `rtc` must show `time_valid=YES` directly from hardware.
- [ ] Mount the BME680 (wiring planned) -> set `BME680_ENABLED 1` in `Sensors.h`.
- [ ] Confirm the battery divider on the bench -> set
      `BATTERY_MONITORING_ENABLED 1` in `Sensors.h`.
- [x] Care KPIs exported in `TelemetryDeep` (proto v4.2, tags 22-27) and
      published by the Hub as the `care` JSON object; also visible from the
      `care` CLI command and the daily `care_day_end` log.
- [ ] Motor current integration (INA219) for stall detection.
- [ ] OTA via the Hub.

## License

MIT: see `LICENSE`.
