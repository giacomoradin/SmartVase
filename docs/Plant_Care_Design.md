# SmartVase — Plant Care Design (product vision and behavioral architecture)

> **Purpose of this document**: define *why* the robot moves, waters and turns
> its lights on — i.e. the autonomous plant-care logic — before (and alongside)
> its code. It is the missing level between the already-implemented primitives
> (navigation, pump, lights) and the goal of the project.
>
> **Status: horizon H1 IMPLEMENTED in Mega firmware v5.3** (see §10 for the
> implementation map and the deliberate deviations). Bench validation pending.
>
> References: `docs/ARCHITECTURE.md` (technical architecture),
> `docs/PINS - Sheet1.csv` (wiring, SSOT), `SmartVase_data_structure.md` (MQTT).
> Author: Giacomo Radin — 2026-07-01 (updated 2026-07-02).

---

## 1. Product thesis

**A normal pot is a sensor without an actuator.** Commercial smart pots
measure light and moisture and send a notification: the actuator is still the
human. SmartVase is the only object in its category that can **choose its
microclimate instead of enduring it**: in a domestic environment light is not
uniform and shifts with the hours and the seasons, and the two main causes of
houseplant death are wrong placement and wrong watering — exactly the two axes
on which SmartVase has wheels and a pump.

> **Vision statement**: *SmartVase autonomously keeps a plant inside its vital
> range of light and water, moving through the environment as the plant itself
> would if it could — and proves it with measurable metrics.*

The behavioral model is not the robot vacuum (which maps and covers space) but
the **plant itself**: phototropism, circadian rhythm, homeostasis. The robot
does not navigate to coordinates: it **climbs gradients** (of light) and
**maintains equilibria** (of water and light budget). This choice is not a
fallback — it is what the hardware can honestly do (see §3) and it is
biologically the right behavior for the object.

### 1.1 Use cases

| Context | Value |
|---|---|
| Home | Plants that survive distracted or vacationing owners (base case). |
| Office | Plant maintenance "belongs to nobody": autonomy + cloud telemetry. |
| Reduced mobility / elderly | Moving heavy pots to follow seasonal light is exactly what they cannot do. |
| Education / research | With the measured light budget (§4.2), quantitative experiments on phototropism and light demand become possible — data, not just demos. |

### 1.2 Verification sentence (the definition of "it works")

*"Given a plant with profile P, the system autonomously keeps the daily light
budget and the soil moisture inside P's range, with no human commands, and
this is verifiable from telemetry."*

Anything that does not make this sentence truer or more measurable is
secondary. It is the criterion for prioritizing every TODO.

---

## 2. Layered behavioral architecture

The existing system reads as a four-level subsumption architecture. Levels 0,
1 and 3 **already existed**; level 2 was the missing piece that gives the
others their purpose (and the reason the system used to feel
"over-engineered": the infrastructure was there, the intention was not).

| Level | Name | What it does | Where it lives | Status |
|---|---|---|---|---|
| **L0** | Reflexes / Safety | WDT, Hub deadman, 60 s pump cap, empty tank, emergency avoid <20 cm, degraded mode | Mega (`SystemStatus`, `Pump`, `Movement` FSM) | ✅ done |
| **L1** | Primitives / Skills | `proportionalDrive`, `wallFollowDrive`, pump dosing, UVA lights, filtered sensor reading, **rotating light scan** | Mega (`NavPolicy.h`, `Movement`, `Pump`, `GrowLight`, `Sensors`) | ✅ done (light scan added in v5.3) |
| **L2** | **Homeostasis / Care** | Decides *when and why* to use the primitives: light budget, autonomous watering, circadian rhythm | Mega (**`CarePolicy.h` + `Care.{h,cpp}`**) | ✅ implemented (v5.3), bench validation pending |
| **L3** | Adaptation / Supervision | Telemetry, history, vision `leaf_health`, human override from the app, setpoint adjustment | Hub + cloud + vision + app | 🟡 infrastructure ready, vision loop open |

Rules between levels:
- A level **commands only the level below it** (L2 never touches PWM: it asks
  L1 to "seek light" / "deliver a dose").
- A lower level **can always override** the one above (L0 stops everything,
  always — no changes to the existing safety mechanisms).
- **L2 lives on the Mega**, not on the Hub: autonomy must survive an absent
  Wi-Fi/Hub (consistent with the degraded-mode and deadman design). The
  Hub/cloud (L3) observes, records and *adjusts the setpoints*; it does not
  drive in real time.
- The app user stops being the pilot (`setMode LIGHT`) and becomes the
  **gardener**: they pick the plant profile and watch the metrics. Manual
  commands remain as a temporary override (autonomy resumes after a grace
  period, or immediately with `care on`).

---

## 3. Honest sensor inventory (what we can really know)

No new electronics required for horizon 1. Every limit has a software
mitigation.

| Sensor | What it really measures | Limit | Mitigation |
|---|---|---|---|
| LDR (A1) | **Relative** brightness at one point, uncalibrated ADC 0..1023 (dark≈11, lab neon≈540) | No direction; scale depends on the room | **Rotating scan** for direction (§5.1); **normalization against the observed daily maximum** for scale (§4.2) |
| Soil fork (A0) | Relative moisture, uncalibrated, slow drift | Not a volumetric % | **Hysteresis between two ADC thresholds** from the profile + dose/soak/verify cycle (§4.3); thresholds bench-tunable from the CLI |
| US4 tank | Distance to the water surface | Tank depth unknown | Already handled: `tank <cm>` configurable, NaN→empty fail-safe |
| 5× nav US | Obstacles 2–400 cm | No odometry/map; US5 faulty (to repair) | Navigation is **taxis** (gradient), not path-planning: distances only prevent collisions. No map required, ever |
| RTC DS3232 | Time of day | CR2032 replaced 2026-07-01 (not yet verified); software fallback clock from 08:00 | Sufficient for the circadian rhythm; verify the chip on the bench (`rtc`) |
| ESP32-CAM | JPEG of the plant | `vision/result` loop not closed | In H2 it becomes the L3 sensor: `leaf_health` + exposure estimate (a second light sensor, from the plant's point of view) |
| Battery (A2) | — (disabled, divider not fitted) | No autonomy estimate | H3. Meanwhile: **relocations-per-day budget** as an energy proxy (§4.4) |

**Fundamental design consequence**: without odometry we never promise "return
to spot X". We promise "find a good spot again" — which is what the plant
needs and is robust by construction (the environment changes, the gradient
does not).

---

## 4. The three vital variables and their control loops

### 4.1 Model: homeostasis

The product is a **closed-loop life-support system**. Three variables, three
loops, each with setpoints from the *plant profile* (§4.5):

| Variable | Sensor | Actuators (in order of preference) | Setpoint from the profile |
|---|---|---|---|
| Cumulative daily light | LDR integrated over time | 1) movement (seek sun/shade) 2) UVA lights (compensator, capped) | `light_target_min` |
| Soil moisture | Fork on A0 | Pump (short doses), tank-protected | `soil_dry_adc` / `soil_wet_adc` |
| Leaf health (H2) | CAM + vision | None directly: **adjusts the setpoints** of the other two loops | `leaf_health` trend |

### 4.2 Light loop: the "light budget" (DLI proxy)

In agronomy the light demand is expressed as **DLI** (Daily Light Integral,
mol/m²/day). The LDR is not calibrated in lux, but *control* only needs a
**consistent** measure, not an absolute one:

- Every care tick accumulates `budget += luxNorm · dt`, where
  `luxNorm = luxAdc / max(dayMaxAdc, LUX_FLOOR)` ∈ [0,1].
- `dayMaxAdc` = maximum observed ADC, rising instantly to any new peak and
  decaying slowly at each daily reset (per-room auto-calibration: in direct
  sun the maximum rises; the floor `CARE_LUX_FLOOR_ADC` keeps a dark room from
  reading as "full light").
- The profile target is expressed in **equivalent full-light minutes** (shade
  plant = 120, medium = 240, sun = 390). Comparable, explainable, measurable.
- The budget resets when the day changes (RTC-anchored).
- The **UVA lights stop being a detached feature**: they become the light
  loop's compensator of last resort — on only if, near the end of the daylight
  window, the budget deficit exceeds 20%, with a daily cap
  (`grow_light_max_min`). This replaces the previous "IDLE + lux<threshold"
  rule while care is active (the legacy rule still applies with care off).

### 4.3 Water loop: dose–soak–verify

Autonomous watering is **not** "pump until wet" (water takes minutes to reach
the fork → drowning risk). Standard horticultural practice:

1. A watering **cycle** starts only when the soil reads drier than
   `soil_dry_adc` (firmware convention: lower ADC = drier).
2. One short **dose** (`dose_ms`, well under the existing 30/60 s caps) → an
   absorption **wait** (`soak_min`) → a **new reading**.
3. Repeat until `soil ≥ soil_wet_adc` (hysteresis band) or the daily cap
   `max_doses_per_day` (fail-safe against a broken/unplugged fork that would
   read "dry" forever).
4. Empty tank (`tankConsideredEmpty`, pre-existing) → skip and log a WARN.

Implementation note (deliberate refinement of the first draft): rather than
fixed check hours, the soil is checked **continuously while the robot is
stationary during the day** (BASK/SHELTER/TOP_UP states) — the soak timer and
the daily dose cap already bound the actuation rate, and the hysteresis band
prevents chattering. All pre-existing safeties (5 s rate-limit, 30 s clamp,
refusal in degraded mode, tank auto-stop mid-dose) remain in charge below
(L0).

### 4.4 Energy loop (proxy, until the battery monitor is wired)

A **relocations-per-day budget** (`max_reloc_per_day`, default 6) plus
anti-nomadism hysteresis (§6): the robot moves the minimum necessary. Positive
side effect: a pot that relocates 3–5 times a day with a purpose is a credible
product; one that wanders is a toy.

### 4.5 Plant profile (`PlantProfile`)

Persisted in `DeviceConfig` (EEPROM dual-slot, existing wear-leveling and
throttling). Three presets (`plant <shade|medium|sun>` from the CLI) +
individually tunable values:

```c
struct PlantProfile {
    uint16_t light_target_min;   // equivalent full-light minutes/day (120/240/390)
    uint16_t lux_high_adc;       // sustained ADC above this -> seek shade (heat proxy; 1024 = never)
    uint16_t soil_dry_adc;       // below = dry -> start a watering cycle (fork ADC)
    uint16_t soil_wet_adc;       // at/above = wet -> stop the cycle (hysteresis)
    uint16_t dose_ms;            // single pump dose duration
    uint8_t  soak_min;           // absorption wait between doses
    uint8_t  max_doses_per_day;  // fork fail-safe
    uint8_t  max_reloc_per_day;  // energy proxy / anti-wandering
    uint8_t  grow_light_max_min; // daily UVA top-up cap
};
```

The dry threshold reuses the pre-existing `soil_dry_threshold` (single source
of truth); the presets are starting points meant to be tuned per plant on the
bench.

---

## 5. New L1 primitives

### 5.1 `lightScan` — the solar compass (the only new motion primitive)

The LDR is a single point: the **direction** of light can only be obtained by
moving. Like a sunflower:

1. In-place rotation at calibration PWM for `LIGHT_SCAN_TOTAL_MS` (~one full
   turn, **time-based — bench-tune against a real 360°**), sampling the ADC
   into 12 time sectors (`M_SCAN_ROTATE`).
2. The best sector is selected (maximum when seeking light, minimum for
   shade; `careBestScanSector`, NaN-safe).
3. A second rotation up to the middle of that sector (time-based dead
   reckoning, `M_SCAN_ALIGN`) — imprecise, but the error is corrected by
   step 4.
4. Forward drive with `proportionalDrive` + the existing `seekBiasPwm` while
   the ADC keeps improving; when it stalls for `CARE_STALL_S` → settle
   (`BASK`) or, on timeout, give up gracefully.

This upgrades the threshold-based seeking (`seekWantsTurn`, bang-bang around
`light_threshold`) into a true **gradient climb**: the robot finds *the
brightest reachable spot*, not "a spot above threshold". Sector selection is
pure (`CarePolicy.h`, host-tested); only the rotate/sample sequence touches
the hardware (`Movement`). Obstacles are not evaluated during the in-place
rotation (no translation); the global 20 s motor safety timeout still bounds
the whole maneuver.

### 5.2 `waterDose` — the dose–soak–verify wrapper

No new hardware: a non-blocking sequence on top of `Pump` (dose → soak timer →
re-read), driven from `Care.cpp`; the decision (`careDoseWanted`) is pure in
`CarePolicy.h`.

---

## 6. The robot's day — care state machine (`CareState`)

A state machine **above** the existing modes (`LIGHT`/`SHADOW`/`IDLE` become
internal outputs of L2, no longer the user's primary input).

```
            daylight window opens (RTC)
 NIGHT ─────────────────────────────► morning decision
   ▲                                      │ budget deficit + relocations left
   │ window closes                        ▼
   │                            ┌──── SEEK_SUN ◄─────────────┐
   │                            │  lightScan + gradient       │ ADC < 70% of the spot's
   │                            │  climb while ADC improves   │ best for > 10 min
   │                            ▼                             │ (and relocations left)
   │                          BASK ───────────────────────────┘
   │                            │ stationary, accumulates budget; soil checked while stationary
   │       budget met, or       │
   │       heat proxy trips     ▼
   │                        SEEK_SHADE ──► SHELTER (stationary in shade)
   │                                          │
   │   < 1 h to window close, budget < 80%    ▼
   └──────────────────────────────────────  TOP_UP  (UVA lights, daily cap)
```

Decision table (the contract implemented by `careStep()` in `CarePolicy.h`
and verified by the host tests):

| # | Condition (priority order) | Action / state |
|---|---|---|
| 0 | L0 active (degraded, deadman) or manual override | Care holds off; L0/operator commands |
| 1 | No reliable clock, or outside the daylight window | `NIGHT`: parked, lights off (fail-safe) |
| 2 | Soil dry (cycle rules §4.3), stationary daylight state, tank ok, doses left | Watering dose (then soak/verify), orthogonal to the states below |
| 3 | Heat proxy (`lux > lux_high_adc` sustained 15 min) **or** budget ≥ 100% | `SEEK_SHADE` → `SHELTER` |
| 4 | Budget < 100% and (first relocation of the day **or** ADC < 70% of the spot's best for > 10 min) and relocations left | `SEEK_SUN` (lightScan + gradient climb) |
| 5 | Budget < 80% at < 1 h from window close, UVA cap not consumed | `TOP_UP` (UVA on) |
| 6 | Otherwise | `BASK`/`SHELTER`: stay put, accumulate |

Hysteresis everywhere (70%/10 min, 80%/1 h, 15 min heat streak) and the
relocation budget: the default behavior is **staying still**. Movement is the
justified exception — anti-nomadism, anti-wear, and legibility for whoever is
watching.

---

## 7. Measurability — KPIs and telemetry

What makes the project a product and not a demo: **numbers**.

| KPI | Where it is visible today | Meaning |
|---|---|---|
| Light budget % | `care` CLI + `care_day_end` daily log | % of the daily target achieved — *the* metric |
| Relocations | `care` CLI + daily log | How much motion work was needed |
| Water doses | `care` CLI + daily log (+ existing `total_irrigations`) | With `soil_moisture` → absorption curve |
| Care state | `care` CLI (`state = BASK` …) | What it is doing and why (debug + app) |
| UVA top-up minutes | `care` CLI + daily log | How much artificial compensation was needed |

The KPIs travel two channels:
- **`TelemetryDeep` fields** (proto v4.1, tags 22-27): the current snapshot,
  published by the Hub inside the telemetry JSON as the `care` object
  (`SmartVase_data_structure.md` §1) — the app's primary source.
- **Logs**: the daily `care_day_end` INFO summary (`b<budget%> d<doses>
  r<relocations> g<uva-minutes>`) plus the discrete events
  (`care_state`, `care_water`, `care_paused`/`care_resumed`), through the
  normal Mega→Hub→MQTT→Firestore log pipeline
  (`SmartVase_data_structure.md` §2.1).

In H2, the CAM capture coordinates with the care FSM: photos in `BASK` (good
light → `frame_quality: ok` almost guaranteed) — which also solves the
dark/blurry frame problem for free.

---

## 8. Roadmap by horizons

### H1 — "The plant takes care of itself" (existing HW only) — ✅ code complete (v5.3)
1. ✅ `CarePolicy.h` (pure: profile, budget, decision table §6) +
   `tests/host/test_care_policy.cpp` (50 checks).
2. ✅ `PlantProfile` in EEPROM + CLI `plant` / `care`.
3. ✅ `lightScan` primitive in `Movement` (FSM states) with pure sector
   selection.
4. ✅ Non-blocking dose/soak/verify watering in `Care.cpp`.
5. ⬜ Pending hardware fixes that gate the bench validation: M1INA wire (D41),
   US5 probe, RTC verification after the CR2032 replacement (2026-07-01).
6. ✅ KPIs in `TelemetryDeep` (proto v4.1, tags 22-27) + `care` object in the
   Hub telemetry JSON, see §7.

**H1 demo**: robot in a room with one lit zone; in the morning (or with a
simulated RTC) it leaves the shade, finds the light, parks, accumulates
budget, retreats to shade at full budget, waters when the fork reads dry.
Reproducible, filmable, measurable from the CLI (`care`).

### H2 — "The loop closes" (cloud + vision)
- Hub subscribes to `vision/result`; the `leaf_health` trend adjusts the
  setpoints (e.g. `warning` with a constantly-met budget → possibly *too
  much* light: lower the target).
- Capture coordinated in `BASK`; tank alarm → app notification; the app shows
  the KPIs as a "plant wellness" ring instead of raw ADC values.
- Real vision pipeline on top of `pixel_analyzer.py` (with Antonio).

### H3 — Long-term vision (new HW, after H1+H2 validation)
- Battery monitor (A2) + real energy-saving behavior.
- Docking: base with charging + tank refill ("go home" via IR beacon or
  wall-following toward a fixed reference — consistent with the no-map rule).
- Seasonal adaptation from the Firestore history (targets update themselves as
  the room's light changes).
- Multi-plant / multi-room.

---

## 9. What we will NOT do (explicit decisions)

- **No SLAM/maps/odometry**: gradient taxis is the right choice for this
  hardware and this purpose, not a fallback. Adding encoders/IMU to "navigate
  better" would not make the plant healthier.
- **No absolute calibration of the analog sensors**: control is relative with
  auto-normalization; fine calibration belongs to the gardener (bench CLI),
  not the firmware.
- **No real-time intelligence on the Hub/cloud**: L2 stays on the Mega; the
  cloud observes and adjusts. The robot must care for the plant with the
  Wi-Fi off.
- **No new features before the verification sentence (§1.2)**: every proposal
  is judged by asking "does it make the sentence truer or more measurable?".

---

## 10. Implementation map (v5.3)

| Design element | Code | Tests |
|---|---|---|
| Plant profiles + presets | `CarePolicy.h` (`PlantProfile`, `carePresetProfile`) + `DeviceConfig` fields + CLI `plant` | `test_care_policy` (presets) |
| Light budget | `CarePolicy.h` (`CareBudget`, `careBudget*`) + `Care.cpp` accounting | `test_care_policy` (budget) |
| Light scan | `Movement` (`startLightScan`, `M_SCAN_ROTATE/ALIGN`) + `careBestScanSector` | `test_care_policy` (sectors); rotation time bench-tuned |
| Dose/soak/verify | `Care.cpp::handleWatering` + `careDoseWanted` | `test_care_policy` (dose hysteresis) |
| Care state machine | `careStep()` (pure) + `Care.cpp::tick/applyOutputs` | `test_care_policy` (decision table, 25 checks) |
| UVA top-up | `careStep` TOP_UP + `GrowLight::force` + main-loop gating | `test_care_policy` (top-up trigger/cap) |
| Manual override | `Care.cpp` (30 min suspension on external `setMode`) | bench |
| KPIs | `TelemetryDeep` tags 22-27 (proto v4.1) + Hub `care` JSON object + `care` CLI + `care_day_end` log | bench (cloud side) |
| Whole-day behavior | — (emergent from the above) | `test_care_day_sim` (3 simulated scenarios) |

Deviations from the first draft, all deliberate: continuous stationary soil
check instead of fixed check hours (§4.3 note); care disabled by default
(bench safety: a freshly flashed robot must not act on its own).
