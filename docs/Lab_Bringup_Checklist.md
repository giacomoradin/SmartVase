# SmartVase — Checklist bring-up in laboratorio

> Procedura passo-passo per il primo flash e collaudo dei tre firmware
> (Mega v5.1, Hub v1.2, CAM v2.1) sul nuovo prototipo.
> L'ordine è pensato per essere **sicuro**: prima si verifica ogni
> attuatore "a vuoto", poi si collega il carico.
>
> Tutto il collaudo funziona **senza rete**: ogni scheda ha una CLI
> seriale a 115200 baud (terminatore di riga: Newline / `\n`).

---

## 0. Materiale da portare

- [ ] Cavo USB-B (Mega) + cavo micro-USB (Hub ESP32 devkit).
- [ ] Per la CAM: adattatore USB-seriale FTDI 3.3V **oppure** base ESP32-CAM-MB.
      Con FTDI: per entrare in flash mode collegare **GPIO0 a GND** e premere
      reset; scollegare GPIO0 e resettare di nuovo per avviare il firmware.
- [ ] **Partitore resistivo o level shifter per la UART Mega→Hub**:
      il TX1 del Mega è a 5V, il GPIO16 dell'ESP32 tollera 3.3V.
      Un partitore 1kΩ/2kΩ è sufficiente (5V · 2/3 ≈ 3.3V).
      La direzione opposta (Hub TX 3.3V → Mega RX) funziona diretta.
- [ ] Tanica + acqua per la taratura di US4 e il test della pompa.
- [ ] Torcia/telefono per il test del fotoresistore.

## 0.1 Comandi base (PowerShell, dalla root del repo)

```powershell
# Build
.\build_mega.bat
.\build_hub.bat
.\build_cam.bat

# Build + flash (aggiungere la porta se il rilevamento automatico fallisce)
.\build_mega.bat -t upload
.\build_hub.bat  -t upload
.\build_cam.bat  -t upload --upload-port COM5

# Lista porte COM disponibili
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device list

# Monitor seriale (CTRL+C per uscire)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor -b 115200 -p COM5
```

> La macchina è risultata **offline verso il registry PlatformIO**: tutte le
> dipendenze sono già in cache o vendorizzate nel repo, le build non hanno
> bisogno di internet. Non lanciare `pio pkg update`.

---

## 1. Arduino Mega da solo (la fase più lunga)

Flash + monitor seriale. Al boot compare `Platform Controller v5.1 boot`.

### 1.1 Setup sessione

- [ ] `standalone on` — **primo comando da dare sempre a banco**: sospende il
      deadman dell'Hub (altrimenti dopo 120 s senza Hub il Mega entra in
      degraded mode e blocca motori e pompa).
- [ ] `version` → v5.1.0.
- [ ] `status` → degradedMode=NO, freeRam ≳ 3500 B.

### 1.2 Sensori a ultrasuoni (uno alla volta)

- [ ] `sensors` ripetuto, mettendo la mano davanti a ciascuna sonda:
      | Sonda | Ruolo | Trig/Echo |
      |-------|-------|-----------|
      | US1   | frontale alto    | 33/35 |
      | US2   | frontale destro  | 26/27 |
      | US3   | frontale sinistro| 36/37 |
      | US4   | tanica           | 50/51 |
      | US5   | lato sinistro    | 4/5   |
      | US6   | lato destro      | 28/29 |
- [ ] Se una sonda legge sempre `nan`: probabile trig/echo invertiti nel
      cablaggio (verifica contro `docs/PINS - Sheet1.csv`) o VCC/GND mancanti.
- [ ] Nota: il filtro EMA impiega ~1 s ad assestarsi dopo un cambiamento.

### 1.3 Sensori analogici — annotare i valori!

- [ ] Forcella (A0): leggere `sensors` con forcella **asciutta** e poi
      **immersa**. Annotare i due valori ADC qui: asciutta=____ bagnata=____
      (servono per tarare `soil_dry_threshold`).
- [ ] Fotoresistore (A1): valore con LDR **coperto** e **illuminato**.
      Annotare: buio=____ luce=____ (serve per `light_threshold` e per capire
      la direzione del partitore: con LDR verso VCC più luce = valore più alto).

### 1.4 RTC DS3232

- [ ] `rtc` → `rtc_ok = YES`. Se NO: controllare SDA=20/SCL=21 e alimentazione.
- [ ] Se `time_valid = NO` (modulo nuovo o batteria tampone scarica):
      ottenere l'epoch corrente dal PC (PowerShell):
      ```powershell
      [DateTimeOffset]::Now.ToUnixTimeSeconds()
      ```
      poi sul Mega: `rtc set <numero>`. Riverificare con `rtc`.

### 1.5 Motori — ROBOT SOLLEVATO da terra

- [ ] `motor f 1000` → entrambe le ruote in avanti. Se una gira al contrario:
      invertire i due fili di quel motore sull'H-bridge (oppure scambiare
      IN1/IN2 o IN3/IN4 in `Movement.cpp` e riflashare).
- [ ] `motor b 1000` / `motor l 1000` / `motor r 1000` → verificare che
      "left/right" corrispondano alla sinistra/destra fisica del robot.
      Se invertiti: scambiare i gruppi LEFT/RIGHT dei `#define` in
      `Movement.cpp` (canale A pin 6/43/45, canale B pin 7/47/49).
- [ ] Calibrazione marcia dritta (si può fare dopo, a terra):
      `calib <left> <right>` (0..255, persiste in EEPROM). Default 255/240.

### 1.6 Relè pompa — POMPA SCOLLEGATA

- [ ] Al boot il relè deve restare **diseccitato** (nessun click all'accensione).
      Se il relè si attiva da solo al boot: il modulo è attivo-alto →
      mettere `PUMP_RELAY_ACTIVE_LOW 0` in `Pump.cpp` e riflashare.
- [ ] `tank` → con US4 non ancora montato sulla tanica dirà
      `SENSOR FAULT -> pompa bloccata`: è il comportamento fail-safe voluto.
- [ ] Per testare il solo relè serve una lettura valida di US4: puntarlo verso
      una superficie a meno della soglia (default 20 cm), poi `pump 1000` →
      un click di attacco e uno di stacco dopo 1 s.

### 1.7 Tanica e protezione pompa

- [ ] Montare US4 sul coperchio della tanica, puntato verso l'acqua.
- [ ] Tanica **piena**: `tank` → annotare la distanza: piena=____ cm.
- [ ] Tanica **vuota** (o quasi): `tank` → annotare: vuota=____ cm.
- [ ] Impostare la soglia ~2 cm sotto il livello minimo utile:
      `tank <vuota - 2>` (es. se vuota=18 → `tank 16`). Persiste in EEPROM.
- [ ] Test protezione: con tanica vuota `pump 2000` → deve rispondere
      `BLOCCATO: tanica vuota`. Riempire → `pump 2000` → la pompa parte.
- [ ] Test auto-stop: avviare `pump 30000` e svuotare/alzare US4 → la pompa
      deve fermarsi da sola (log `pump_stop tank_empty_or_fault`).
- [ ] Solo ora collegare la pompa al relè e ripetere con acqua.

---

## 2. ESP32 Hub da solo

Flash + monitor. Al boot: `[SmartVase Hub] Avvio... v1.2` poi `[CLI] pronta`.

- [ ] Senza Wi-Fi configurato il boot prosegue **offline** (è il comportamento
      atteso: nessun AP di provisioning, nessun blocco).
- [ ] `version` → 1.2.0. `status` → wifi OFFLINE, mqtt NON CONFIGURATO,
      mega_link ASSENTE: tutto normale a questo punto.
- [ ] (Solo quando ci sarà rete) provisioning dalla CLI:
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

## 3. Catena Hub ↔ Mega (il test più importante)

Cablaggio (a schede spente):

| Da (Mega)   | A (Hub ESP32) | Nota                              |
|-------------|---------------|-----------------------------------|
| TX1 = D18   | GPIO16 (RX2)  | **⚠ tramite partitore 5V→3.3V**  |
| RX1 = D19   | GPIO17 (TX2)  | diretto (3.3V è letto come HIGH)  |
| GND         | GND           | massa comune obbligatoria         |

- [ ] Accendere entrambi, monitor sull'**Hub**.
- [ ] Entro ~5 s devono comparire i log del Mega inoltrati (`Mega log [INFO] boot...`).
- [ ] `status` sull'Hub → `mega_link = OK (ultimo msg N s fa)`.
- [ ] `telemetry` → distanze/lux/soil correnti lette dal Mega: muovere una mano
      davanti a una sonda e rilanciare per conferma.
- [ ] `soil` → deve arrivare `[ACK Mega] ... detail=soil_moisture value=<adc>`.
- [ ] `water 2000` (tanica OK) → ACK `water_started` e pompa attiva 2 s.
- [ ] `water 2000` (tanica vuota) → ACK `ERROR detail=tank_empty`.
- [ ] Sul Mega: con l'Hub collegato l'heartbeat arriva ogni 30 s, quindi
      `standalone off` e verificare con `status` che NON entri in degraded.
- [ ] Spegnere l'Hub e attendere 2 min: il Mega deve entrare in degraded mode
      (`hub_missing`) e fermare tutto → riaccendere l'Hub → recupero automatico.

## 4. ESP32-CAM

Flash (GPIO0→GND per il bootloader se si usa FTDI) + monitor.

- [ ] Boot: `SmartVase Vision Co-Processor v2.1`, poi `CLI pronta`.
- [ ] `status` → `camera = OK`. Se FAILED: controllare che il flat cable
      della OV2640 sia inserito bene (causa n.1) e alimentare a 5V ≥ 500 mA.
- [ ] `capture` → `frame ok: NNNN byte, crc32=..., NN ms`. Ripetere 4-5 volte
      e controllare con `stats` che `failed_frames` non cresca.
- [ ] (Solo quando ci sarà rete + Cloud Function) `set upload_url <...>` +
      credenziali wifi/mqtt come per l'Hub, poi `upload` per il test completo.

---

## 5. Troubleshooting rapido

| Sintomo | Causa probabile | Azione |
|---|---|---|
| Sonda US sempre `nan` | trig/echo invertiti o non alimentata | verifica cablaggio col CSV |
| Tutte le sonde `nan` | GND comune mancante | controlla le masse |
| `pump` sempre BLOCCATO | US4 non vede l'acqua / soglia bassa | `tank` per diagnosi, `tank <cm>` per tarare |
| Relè eccitato al boot | modulo attivo-alto | `PUMP_RELAY_ACTIVE_LOW 0` in Pump.cpp |
| Motore gira al contrario | polarità motore | inverti i fili sull'H-bridge |
| Mega in degraded a banco | deadman Hub scattato | `standalone on` |
| Hub non vede il Mega | TX/RX non incrociati o partitore mancante | rivedi tabella §3 |
| `epoch_s = 0` in telemetria | RTC non impostato | `rtc set <epoch>` (§1.4) |
| CAM riavvii continui | alimentazione insufficiente | 5V dedicati, non dal FTDI |
| upload/flash CAM fallisce | GPIO0 non a GND durante il flash | rivedi §4 |

## 6. A fine giornata

- [ ] Annotare su questo file (o su `docs/SmartVase_Project_State.md`):
      valori forcella/LDR, distanze tanica piena/vuota, soglia `tank` scelta,
      mapping motori verificato, polarità relè.
- [ ] Se è stato modificato un flag (`PUMP_RELAY_ACTIVE_LOW`, pin scambiati):
      commit della modifica con una riga di motivazione.
