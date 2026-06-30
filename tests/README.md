# SmartVase — Test

## `tests/host/` — Unit test nativi (g++)

Test della **logica pura** del firmware compilata ed eseguita sul PC con `g++`
(host), **senza hardware**. Pensati per i giorni in cui il lab e' chiuso: danno
una rete di sicurezza statica prima del prossimo collaudo a banco.

### Perche' g++ e non `pio test`
Su questa macchina (offline verso il registry PlatformIO) **non** sono in cache
ne' la piattaforma `native` ne' `cppcheck`, quindi `pio test -e native` e
`pio check` non sono eseguibili offline. C'e' invece `g++` (Strawberry/MinGW),
che basta per compilare i moduli puri includendo il **codice reale** del
firmware tramite uno shim minimale di `Arduino.h` (`tests/host/stubs/`).

### Come si eseguono
```bash
# da bash (Git Bash):
bash tests/host/run.sh
```
```bat
REM da PowerShell/cmd (richiede g++ nel PATH, es. C:\Strawberry\c\bin):
tests\host\run.bat
```

### Cosa c'e' ora
- `test_crc16.cpp` — CRC16-CCITT: vettori noti (XMODEM) + **invariante di
  protocollo** "Mega e Hub calcolano lo stesso CRC" (un loro disallineamento
  in passato aveva reso muto il link seriale).

### Come aggiungere un test
1. Crea `tests/host/test_<nome>.cpp` con un `int main()` che ritorna 0 se ok.
2. Includi il modulo reale (`#include "../../firmware/.../src/<File>.cpp"`); se
   tira dentro `<Arduino.h>`, lo shim lo soddisfa (estendilo se serve un tipo).
3. Aggiungi `run_one test_<nome>` in `run.sh` e `call :run_one test_<nome>` in `run.bat`.

Candidati futuri (richiedono di isolare la logica dall'HW): soglia tanica,
rate-limit/no-op comandi, costruzione JSON telemetria (ArduinoJson e' header-only).

## Quando si torna online
Si puo' affiancare il flusso PlatformIO ufficiale:
- `pio test -e native` con Unity (aggiungere un `[env:native]` + cartella `test/`).
- `pio check` (cppcheck/clang-tidy) per l'analisi statica dei tre firmware.
Entrambi scaricano pacchetti al primo uso: vanno lanciati con rete disponibile.
