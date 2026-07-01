# SmartVase — Tests

## `tests/host/` — Native unit tests (g++)

Tests of the firmware's **pure logic**, compiled and run on the PC with `g++`
(host), **without hardware**. Meant for the days when the lab is closed: they
provide a static safety net before the next bench test session.

### Why g++ and not `pio test`

On this machine (offline w.r.t. the PlatformIO registry) **neither** the
`native` platform **nor** `cppcheck` are cached, so `pio test -e native` and
`pio check` are not runnable offline. `g++` (Strawberry/MinGW) is available
instead, and is enough to compile the pure modules by including the
firmware's **real code** through a minimal `Arduino.h` shim (`tests/host/stubs/`).

### How to run them

```bash
# from bash (Git Bash):
bash tests/host/run.sh
```

```bat
REM from PowerShell/cmd (requires g++ in the PATH, e.g. C:\Strawberry\c\bin):
tests\host\run.bat
```

### What's here now

- `test_crc16.cpp` — CRC16-CCITT: known vectors (XMODEM) + a **protocol
  invariant** ("the Mega and the Hub compute the same CRC"; a mismatch
  between them once made the serial link go silent).
- `test_crc_utils.cpp` — Hub-side CRC utilities: CCITT (poly `0x1021`,
  0x31C3 test vector) and IBM (0xBB3D test vector) variants.
- `test_command_policy.cpp` — pure command policies (`CommandPolicy.h`):
  water rate-limit, motion-param no-op, duration/param clamping.
- `test_sensor_policy.cpp` — pure sensor policies (`SensorPolicy.h`):
  tank fail-safe, light/shadow seeking direction, grow-light gating,
  daylight window.
- `test_persistence.cpp` — dual-slot EEPROM wear-leveling (`Persistence`):
  slot selection on load, write throttling, no-op on unchanged writes.

### How to add a test

1. Create `tests/host/test_<name>.cpp` with an `int main()` that returns 0 on success.
2. Include the real module (`#include "../../firmware/.../src/<File>.cpp"`); if
   it pulls in `<Arduino.h>`, the shim satisfies it (extend it if a type is missing).
3. Add `run_one test_<name>` in `run.sh` and `call :run_one test_<name>` in `run.bat`.

Future candidates (require isolating the logic from the HW): telemetry
JSON construction (ArduinoJson is header-only).

## When back online

The official PlatformIO flow can be added alongside this one:

- `pio test -e native` with Unity (add an `[env:native]` + a `test/` folder).
- `pio check` (cppcheck/clang-tidy) for static analysis of the three firmwares.

Both download packages on first use: they must be run with network access available.
