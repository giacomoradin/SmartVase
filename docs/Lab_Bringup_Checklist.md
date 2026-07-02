# SmartVase — Lab bring-up checklist

> Step-by-step procedure for the first flash and testing of the three
> firmwares (Mega v5.3, Hub v1.3, CAM v2.1) on the new prototype.
> The order is designed to be **safe**: each actuator is first verified
> "unloaded", and only then is the load connected.
>
> The whole test works **without networking**: every board has a serial
> CLI at 115200 baud (line terminator: Newline / `\n`).

---

## 0. What to bring

- [ ] USB-B cable (Mega) + micro-USB cable (Hub ESP32 devkit).
- [ ] For the CAM: an FTDI 3.3V USB-serial adapter **or** an ESP32-CAM-MB base.
      With FTDI: to enter flash mode connect **GPIO0 to GND** and press
      reset; disconnect GPIO0 and reset again to boot the firmware.
- [ ] **Resistive divider or level shifter for the Mega→Hub UART**:
      the Mega's TX1 is at 5V, the ESP32's GPIO16 tolerates 3.3V.
      A 1kΩ/2kΩ divider is enough (5V · 2/3 ≈ 3.3V).
      The opposite direction (Hub TX 3.3V → Mega RX) works directly.
- [ ] Tank + water for calibrating US4 and testing the pump.
- [ ] Flashlight/phone for testing the photoresistor.

## 0.1 Basic commands (PowerShell, from the repo root)

```powershell
# Build
.\build_mega.bat
.\build_hub.bat
.\build_cam.bat

# Build + flash (add the port if auto-detection fails)
.\build_mega.bat -t upload
.\build_hub.bat  -t upload
.\build_cam.bat  -t upload --upload-port COM5

# List available COM ports
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device list

# Serial monitor (CTRL+C to quit)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor -b 115200 -p COM5
```

> The machine turned out to be **offline w.r.t. the PlatformIO registry**: all
> dependencies are already cached or vendored in the repo, the builds do not
> need internet. Do not run `pio pkg update`.

---

## 1. Arduino Mega alone (the longest phase)

Flash + serial monitor. At boot, `Platform Controller v5.1 boot` appears.

### 1.1 Session setup

- [ ] `standalone on` — **the first command to always give on the bench**: it
      suspends the Hub's deadman (otherwise, after 120 s without the Hub, the
      Mega enters degraded mode and stops the motors and pump).
- [ ] `version` → v5.1.0.
- [ ] `status` → degradedMode=NO, freeRam ≳ 3500 B.

### 1.2 Ultrasonic sensors (one at a time)

- [ ] Repeat `sensors`, placing a hand in front of each probe:
      | Probe | Role | Trig/Echo |
      |-------|------|-----------|
      | US1   | front-top     | 33/35 |
      | US2   | front-right   | 26/27 |
      | US3   | front-left    | 36/37 |
      | US4   | tank          | 50/51 |
      | US5   | left side     | 4/5   |
      | US6   | right side    | 28/29 |
- [ ] If a probe always reads `nan`: trig/echo are likely swapped in the
      wiring (check against `docs/PINS - Sheet1.csv`) or VCC/GND is missing.
- [ ] Note: the EMA filter takes ~1 s to settle after a change.

### 1.3 Analog sensors — write down the values!

- [ ] Fork (A0): read `sensors` with the fork **dry** and then **immersed**.
      Write down the two ADC values here: dry=____ wet=____
      (needed to calibrate `soil_dry_threshold`).
- [ ] Photoresistor (A1): value with the LDR **covered** and **lit**.
      Write down: dark=____ light=____ . Observed real scale ~0-800 (dark≈11,
      lab neon≈540). Calibrate `light_threshold` (default **500**) with
      `light <adc>`: the "sufficient" ambient-light value must stay ABOVE
      the threshold (UVA lights off), darkness BELOW it (UVA lights on). The
      threshold is shared with the LIGHT/SHADOW seeking.

### 1.4 RTC DS3232

- [ ] `rtc` → `chip_ok = YES`. If NO: check SDA=20/SCL=21 and power.
- [ ] If `time_valid = NO` (new module or dead backup battery):
      get the current epoch from the PC (PowerShell):
      ```powershell
      [DateTimeOffset]::Now.ToUnixTimeSeconds()
      ```
      then on the Mega: `rtc set <number>`. Re-check with `rtc`.
- [ ] **Dead/absent CR2032 battery?** The firmware automatically enables a
      **software fallback clock**: at boot it starts from 08:00 and `rtc`
      shows `fake_clock = YES`, `time_valid = YES`. This is fine for bench
      testing (it is lost on every reset, must be re-set). The UVA lights
      depend on the time: with `time_valid = NO` they stay off forever.

### 1.5 Motors — Pololu Dual VNH5019 — ROBOT LIFTED off the ground

- [ ] 🔴 **FIRST OF ALL — common GND**: with a multimeter, verify continuity
      (0Ω) between Mega GND, the shield's (logic) GND and the battery −.
      Without a common ground the signals do not arrive and the outputs stay
      at **0V** even with VDD present (cause #1).
- [ ] Pin mapping (see `docs/PINS - Sheet1.csv`): D6→M1PWM, D43→M1INA,
      D45→M1INB, D7→M2PWM, D47→M2INA, D49→M2INB. ⚠️ On the standard shield
      pin 6 is M1EN, not M1PWM: make sure the wire really goes to **M1PWM**.
- [ ] `diag` → the `fault L/R` line: must read `ok/ok`. If `!!FAULT` on a
      motor, the VNH5019 is in protection mode (EN/DIAG low): check the
      power/wiring of that channel. (EN/DIAG on D41/D39 is optional.)
- [ ] `motor f 60000` → both wheels forward (test up to 60 s). If one turns
      the wrong way: swap the two wires of that motor, **or** swap
      INA/INB of that motor in `Movement.cpp` and reflash.
- [ ] `motortest` → guided f/b/l/r sequence; verify that "left/right"
      match the physical left/right side. If swapped: swap the LEFT/RIGHT
      `#define` groups in `Movement.cpp` (L: 6/43/45/41, R: 7/47/49/39).
- [ ] If the outputs stay at 0V with a correct common GND and mapping: measure
      with the multimeter D43≈5V / D45≈0V / D6≈5V during `motor f 60000`, then
      M1EN≈5V (VDD, via the pull-up). Check the shield's "M1EN A=B" / "M2EN A=B"
      jumper.
- [ ] Straight-drive calibration (can be done later, on the ground):
      `calib <left> <right>` (0..255, persists in EEPROM). Default 255/240.

### 1.6 Pump relay — PUMP DISCONNECTED

- [ ] At boot the relay must remain **de-energized** (no click at power-on).
      If the relay activates on its own at boot: the module is active-high →
      set `PUMP_RELAY_ACTIVE_LOW 0` in `Pump.cpp` and reflash.
- [ ] `tank` → with US4 not yet mounted on the tank it will say
      `SENSOR FAULT -> pump blocked`: this is the intended fail-safe behavior.
- [ ] To test the relay alone you need a valid US4 reading: point it at
      a surface closer than the threshold (default 20 cm), then `pump 1000` →
      one click to engage and one to disengage after 1 s.

### 1.7 Tank and pump protection

- [ ] Mount US4 on the tank lid, pointed at the water.
- [ ] Tank **full**: `tank` → write down the distance: full=____ cm.
- [ ] Tank **empty** (or nearly so): `tank` → write down: empty=____ cm.
- [ ] Set the threshold ~2 cm below the minimum usable level:
      `tank <empty - 2>` (e.g. if empty=18 → `tank 16`). Persists in EEPROM.
- [ ] Protection test: with an empty tank, `pump 2000` → must respond
      `BLOCKED: tank empty`. Fill it → `pump 2000` → the pump starts.
- [ ] Auto-stop test: start `pump 30000` and empty/lift US4 → the pump
      must stop by itself (log `pump_stop tank_empty_or_fault`).
- [ ] Only now connect the pump to the relay and repeat with water.

### 1.8 UVA grow lights (relay D11, NC contact)

> The lights are wired on the **NC** contact of the 2nd relay channel: with
> the relay at rest they are **ON**. They turn on automatically only if
> `IDLE` + `lux<threshold` + the time is within 06:00–20:00. Use the `diag`
> `[LUCI UVA]` line to read the state.

- [ ] `mode idle` + LDR covered (dark) + `time_valid=YES` → `diag` must show
      `growLight=ON` (and the lights physically on).
- [ ] Shine light on the LDR above the threshold (or `mode light`) →
      `growLight=OFF`.
- [ ] If the lights stay on all the time in full light: the threshold is too
      high → set `light <adc>` lower than the ambient light value (see §1.3).
- [ ] Check the time window: if `time_valid=NO` the lights stay off forever
      (no time available); set `rtc set <epoch>` or rely on the 08:00 fallback.

### 1.9 Autonomous care layer (Mega v5.3, `care on`)

> Full behavior description and decision table: `docs/Plant_Care_Design.md`.
> ⚠️ First boot of v5.3: the extended `DeviceConfig` invalidates the stored
> EEPROM config (CRC) → factory defaults. Re-apply `calib`, `tank`, `light`
> before this section. Prerequisites: `standalone on` (no Hub), a valid time
> (`rtc` → `time_valid=YES` — verify the new CR2032 here: it must NOT need the
> software fallback), pump and motors already verified in §1.5–1.7.

- [ ] **Scan rotation calibration**: `motor l 6000` with wheels on the ground —
      the robot should complete roughly ONE full turn. If not, adjust
      `LIGHT_SCAN_TOTAL_MS` in `Movement.cpp` (time-based, no encoders) and
      re-flash. The light scan depends on it.
- [ ] `plant medium` (or the preset matching the test plant) → `plant` shows
      the profile; `config` shows `care_enabled=NO`.
- [ ] `care on` → `care` shows `state = NIGHT` or the morning decision within
      ~1 s ticks; with a light source on one side, the robot should perform the
      scan rotation (`status` → `movementState=SCAN_ROTATE/SCAN_ALIGN`), then
      drive toward the light and settle (`state = BASK`).
- [ ] `care` KPIs: `light_budget` must grow while basking under light;
      `day_max_adc` must match the brightest reading seen.
- [ ] Re-seek test: shade the LDR at the basking spot for >10 min (or lower
      `light <adc>`… patience) → the robot relocates (relocations counter +1).
      For a quick bench check it is acceptable to just verify that covering
      the LDR does NOT cause an immediate relocation (10 min hysteresis).
- [ ] Watering: with the fork in dry soil (ADC below `soil_dry_adc`) and the
      robot stationary → one dose of `dose_ms`, then `care` shows
      `soak_remaining_s` counting down; no second dose before the soak ends.
      Tank empty → WARN `care_water tank_empty`, no dose.
- [ ] Manual override: `mode light` while care is active → `status` shows
      `careActive=PAUSED (manual override)`; care resumes alone after 30 min
      (or immediately with `care on`).
- [ ] `care off` → the robot stops (target mode IDLE) and the UVA lights go
      back to the legacy rule of §1.8.

---

## 2. ESP32 Hub alone

Flash + monitor. At boot: `[SmartVase Hub] Starting... v1.2` then `[CLI] ready`.

- [ ] Without Wi-Fi configured, the boot proceeds **offline** (this is the
      expected behavior: no provisioning AP, no blocking).
- [ ] `version` → 1.2.0. `status` → wifi OFFLINE, mqtt NOT CONFIGURED,
      mega_link ABSENT: all normal at this point.
- [ ] (Only once there is networking) provisioning from the CLI:
      ```
      set wifi_ssid <ssid>
      set wifi_pass <password>
      set mqtt_broker <xxx>.s1.eu.hivemq.cloud
      set mqtt_port 8883
      set mqtt_user <user>
      set mqtt_pass <pass>
      save
      reboot
      ```

## 3. Hub ↔ Mega chain (the most important test)

Wiring (with the boards powered off):

| From (Mega) | To (Hub ESP32) | Note                               |
|-------------|-----------------|-------------------------------------|
| TX1 = D18   | GPIO16 (RX2)    | **⚠ via a 5V→3.3V divider**        |
| RX1 = D19   | GPIO17 (TX2)    | direct (3.3V is read as HIGH)       |
| GND         | GND             | common ground mandatory             |

- [ ] Power both on, monitor on the **Hub**.
- [ ] Within ~5 s the forwarded Mega logs must appear (`Mega log [INFO] boot...`).
- [ ] `status` on the Hub → `mega_link = OK (last msg N s ago)`.
- [ ] `telemetry` → current distances/lux/soil read from the Mega: move a hand
      in front of a probe and rerun to confirm.
- [ ] `soil` → must receive `[ACK Mega] ... detail=soil_moisture value=<adc>`.
- [ ] `water 2000` (tank OK) → ACK `water_started` and the pump active for 2 s.
- [ ] `water 2000` (empty tank) → ACK `ERROR detail=tank_empty`.
- [ ] On the Mega: with the Hub connected the heartbeat arrives every 30 s, so
      `standalone off` and verify with `status` that it does NOT enter degraded mode.
- [ ] Power off the Hub and wait 2 min: the Mega must enter degraded mode
      (`hub_missing`) and stop everything → power the Hub back on → automatic recovery.

## 4. ESP32-CAM

Flash (GPIO0→GND for the bootloader if using FTDI) + monitor.

- [ ] Boot: `SmartVase Vision Co-Processor v2.1`, then `CLI ready`.
- [ ] `status` → `camera = OK`. If FAILED: check that the OV2640's flat
      cable is properly seated (cause #1) and power it at 5V ≥ 500 mA.
- [ ] `capture` → `frame ok: NNNN bytes, crc32=..., NN ms`. Repeat 4-5 times
      and check with `stats` that `failed_frames` does not grow.
- [ ] (Only once there is networking + the Cloud Function) `set upload_url <...>`
      + wifi/mqtt credentials as for the Hub, then `upload` for the full test.

---

## 5. Quick troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| US probe always `nan` | trig/echo swapped or unpowered | check the wiring against the CSV |
| All probes `nan` | common GND missing | check the grounds |
| `pump` always BLOCKED | US4 does not see the water / threshold too low | `tank` to diagnose, `tank <cm>` to calibrate |
| Pump relay energized at boot | active-high module | `PUMP_RELAY_ACTIVE_LOW 0` in Pump.cpp |
| **Motors: 0V on M1A/M1B/M2A/M2B** | **common GND Mega↔shield missing** | check the ground with a multimeter (§1.5) — cause #1 |
| Motors: outputs 0V, GND ok | wrong PWM/INA/INB pin mapping | check D6→M1PWM etc. (§1.5) |
| `diag` shows `fault !!FAULT` | VNH5019 in protection | power/wiring of the channel, EN A=B jumper |
| Motor turns the wrong way | motor polarity | swap the 2 wires or INA/INB in Movement.cpp |
| Mega in degraded mode on the bench | Hub deadman tripped | `standalone on` |
| Hub does not see the Mega | TX/RX not crossed or missing divider | review the table in §3 |
| UVA lights always on | threshold too high / `time_valid=NO` | lower `light <adc>` / `rtc set` (§1.8) |
| `epoch_s = 0` in telemetry | RTC not set | `rtc set <epoch>` or the 08:00 fallback (§1.4) |
| MQTT command not executed | wrong publish topic | use the full `smartvase/HUB_123456/command/<type>` |
| CAM keeps rebooting | insufficient power | dedicated 5V, not from the FTDI |
| CAM upload/flash fails | GPIO0 not tied to GND during flashing | review §4 |

## 6. At the end of the day

- [ ] Write down in this file (or in `docs/SmartVase_Project_State.md`):
      fork/LDR values, full/empty tank distances, the chosen `tank` threshold,
      verified motor mapping, relay polarity.
- [ ] If a flag was changed (`PUMP_RELAY_ACTIVE_LOW`, swapped pins):
      commit the change with a one-line rationale.
