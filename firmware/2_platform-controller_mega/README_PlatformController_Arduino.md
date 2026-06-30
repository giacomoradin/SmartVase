# SmartVase - Firmware Platform Controller (Arduino Mega)

Firmware del **Platform Controller** SmartVase. Versione **5.2** (working tree):
oltre all'hardening v5.1 (protezione tanica pompa, CLI estesa, modalità
standalone, driver locali HC-SR04/DS3232), la v5.2 aggiunge rate-limit
irrigazione, no-op EEPROM su `setMotionParams`, EMA su lux/soil, anti-circling
del seeking e i contatori seeking/escape in `TelemetryDeep`.
Riferimento architetturale **autoritativo**: `docs/ARCHITECTURE.md`.
PIN map: `docs/PINS - Sheet1.csv`.

## Architettura

Il Mega è "il braccio": pilota direttamente l'hardware (motori, pompa,
sensori, RTC) e parla **solo** con l'ESP32 Hub via Serial1 a 115200 baud
(framing Protobuf+CRC16).

Moduli (`src/`):

| File              | Responsabilità                                                                           |
|-------------------|------------------------------------------------------------------------------------------|
| `main.cpp`        | Setup + loop non bloccante, scheduler telemetria/heartbeat/log, WDT, degraded mode       |
| `Sensors.{h,cpp}` | 6 HC-SR04 (round-robin), RTC DS3232, forcella umidità, fotoresistore, BME680 (flag)      |
| `Movement.{h,cpp}`| State machine motori (IDLE/MOVING/AVOID*/STUCK), light-seek / shadow-seek                |
| `Pump.{h,cpp}`    | Pompa irrigazione non-bloccante (relè D10, max 60s safety)                               |
| `Persistence.{h,cpp}` | EEPROM dual-slot con magic+CRC16, wear leveling                                       |
| `Communication.{h,cpp}` | Framing seriale SOF/len/payload/CRC16, parser stato, log queue, dispatcher comandi |
| `Cli.{h,cpp}`     | CLI di debug su Serial USB (provisioning soglie, test motori/pompa, standalone)          |
| `Ultrasonic.{h,cpp}` | Driver HC-SR04 minimale locale (pulseIn con timeout, nessun delay fisso)              |
| `RtcDs3232.{h,cpp}`  | Driver DS3232 minimale locale (get/set epoch + flag OSF via Wire)                     |
| `SystemStatus.h`  | Struct condivisa di stato (degraded mode, standalone, deviceId, versione fw)             |
| `smartvase_aliases.h` | Typedef/define per i simboli nanopb + tipi C++ interni                               |

## PIN map autoritativo

Vedi `docs/PINS - Sheet1.csv`. Sintesi:

| Periferica           | Pin                             |
|----------------------|---------------------------------|
| US1 (front-top)      | trig D33 / echo D35             |
| US2 (front-right)    | trig D26 / echo D27             |
| US3 (front-left)     | trig D36 / echo D37             |
| US4 (water tank)     | trig D50 / echo D51             |
| US5 (left)           | trig D4  / echo D5              |
| US6 (right)          | trig D28 / echo D29             |
| Motori H-bridge L    | IN1=D43, IN2=D45, ENA=D6 (PWM)  |
| Motori H-bridge R    | IN3=D47, IN4=D49, ENB=D7 (PWM)  |
| Relè pompa           | IN1=D10 (active-low), backup D11|
| RTC DS3232 (I²C)     | SDA=D20, SCL=D21 (addr 0x68)    |
| Forcella umidità     | A0                              |
| Fotoresistore (LDR)  | A1 (spostato da A0)             |
| Batteria (partitore) | A2 (disabilitato finché non cablato) |
| BME680 (I²C)         | addr 0x76                       |

Le costanti pin sono centralizzate in `Sensors.cpp` e `Movement.cpp`.

## Dipendenze (PlatformIO)

```ini
lib_deps =
    adafruit/Adafruit BME680 Library @ ^2.0.1
    paulstoffregen/Time @ ^1.6.1
```

HC-SR04 e DS3232 sono gestiti da driver locali in `src/` (`Ultrasonic`,
`RtcDs3232`): nessun download dal registry, le build funzionano offline.
I file Nanopb (`pb_*.c/h`, `smartvase.pb.{c,h}`) sono già in `src/` e
vengono compilati con lo sketch.

## Build

```
build_mega.bat
```

Equivalente a `pio run -d firmware/2_platform-controller_mega/...`.

## Funzionalità chiave

- **Non-blocking**: nessun `delay()` nel main loop (ad eccezione di
  `Movement::testMove`, usato solo dalla CLI manuale).
- **Hardware Watchdog**: `WDTO_4S`. Reset count salvato in EEPROM stats.
- **Degraded mode**: si attiva se `freeRam() < 800 B` o se l'Hub tace
  >120 s. Ferma motori, ferma pompa, ignora comandi di movimento.
- **EEPROM dual-slot**: doppio slot (alternato) con magic number + CRC16
  per `DeviceConfig` (60 s throttle) e `CumulativeStats` (300 s throttle).
- **Framing seriale**: `SOF=0xAA | len(2) | payload | crc16(2)`, CRC-CCITT
  (poly `0x1021`).
- **Log queue**: circolare a 20 slot, drenata a 200 ms dal main loop.
- **Protezione tanica (v5.1)**: la pompa non parte (e si ferma da sola) se
  US4 misura una distanza oltre `tank_empty_cm` (default 20, tarabile con
  `tank <cm>` da CLI) **o** se la lettura non è valida — fail-safe contro
  la marcia a secco. Vale per comando remoto `water` e CLI `pump`.
- **Modalità standalone (v5.1)**: `standalone on` da CLI sospende il deadman
  dell'Hub per i test a banco senza ESP32 collegato.
- **Comandi supportati** (da Hub):
  `WaterCommand`, `SetModeCommand`, `StopCommand`,
  `RequestDiagnosticsCommand`, `SetMotionParamsCommand`,
  `ReadSoilCommand`, `SoftResetCommand`. Ogni comando produce un
  `CommandResponse` (status OK/ERROR, detail, value, cmd_id, exec_time_ms).

## CLI di debug (Serial USB, 115200, newline)

`help` mostra il menu completo: `status`, `stats`, `config`, `sensors`,
`tank [cm]`, `rtc [set <epoch>]`, `mode <idle|light|shadow>`,
`motor <f|b|l|r> <ms>`, `calib <l> <r>`, `pump <ms>`,
`standalone <on|off>`, `version`, `reboot`.
La procedura completa di collaudo è in `docs/Lab_Bringup_Checklist.md`.

## TODO aperti

- [ ] Confermare partitore batteria a banco → settare
      `BATTERY_MONITORING_ENABLED 1` in `Sensors.h`.
- [ ] Montare il BME680 (oggi assente dal prototipo) → settare
      `BME680_ENABLED 1` in `Sensors.h`.
- [ ] Integrazione corrente motori (INA219) per stallo.
- [ ] OTA via Hub.

## Licenza

MIT — vedi `LICENSE`.
