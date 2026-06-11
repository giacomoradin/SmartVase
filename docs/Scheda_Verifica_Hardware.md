# SmartVase — Scheda di verifica hardware e documentazione sensori

> **Come usare questo documento**: portalo in laboratorio (stampato o sul
> portatile) e **compila i campi `→ ____`** col prototipo sotto mano.
> È diviso in tre parti:
> 1. **Informazioni mancanti** — ciò che il firmware/documentazione assume
>    ma nessuno ha mai verificato sul prototipo reale.
> 2. **Schede componenti** — come funziona ogni sensore/attuatore, specifiche
>    da datasheet, insidie note e cosa controllare (fonti online in fondo a
>    ogni scheda).
> 3. **Checklist di test pratici** — prove con esito atteso e campo per il
>    valore rilevato.
>
> La **procedura operativa** (ordine dei passi, comandi CLI, flash) è in
> [`Lab_Bringup_Checklist.md`](Lab_Bringup_Checklist.md): i due documenti si
> usano insieme — quello dice *come*, questo dice *cosa annotare*.

---

## 1. Informazioni mancanti da raccogliere col prototipo in mano

### 1.1 Architettura di alimentazione ⚡ (la lacuna più grossa)

Non è documentata da nessuna parte. Senza questo quadro non si possono
diagnosticare brownout, reset spontanei o letture ADC instabili.

| Domanda | Rilevato |
|---|---|
| Cosa alimenta il Mega? (USB / Vin / 5V diretto) | → ____ |
| Cosa alimenta l'ESP32 Hub? | → ____ |
| Cosa alimenta la ESP32-CAM? (serve 5V ≥ 2A dedicati) | → ____ |
| Tensione di alimentazione motori (Vs dell'H-bridge) | → ____ V |
| La pompa da dove prende i suoi V? (mai dal 5V del Mega) | → ____ |
| Esiste una batteria principale? Chimica/tensione/capacità | → ____ |
| Tutte le masse sono in comune? (verifica con multimetro in continuità) | → SÌ / NO |
| Jumper 5V dell'H-bridge inserito? (lecito solo se Vs ≤ 12V) | → SÌ / NO |
| Il 5V-out dell'H-bridge alimenta qualcosa? | → ____ |

### 1.2 Identificazione moduli (foto + sigla per ognuno)

| Componente | Da identificare | Rilevato |
|---|---|---|
| Driver motori | Modello (presunto L298N — verifica la sigla sul chip) | → ____ |
| Motori DC | Modello, tensione nominale, riduttore, corrente di stallo se nota | → ____ |
| Modulo relè | Sigla relè (es. SRD-05VDC-SL-C), 1 o 2 canali, jumper JD-VCC presente? | → ____ |
| Pompa | Modello, tensione (tipico 3–6V), corrente, diodo flyback già a bordo? | → ____ |
| Forcella umidità | Puntali nudi su partitore o modulo comparatore (LM393) con uscita AO? | → ____ |
| Fotoresistore | LDR nudo (GL5528?) o modulo KY-018? Resistenza fissa del partitore? | → ____ |
| Modulo RTC | DS3231 o DS3232? Batteria CR2032 presente e che tensione ha? | → ____ |
| ESP32-CAM | Programmatore disponibile: FTDI 3.3V o base MB? | → ____ |
| UART Mega↔Hub | Partitore/level shifter 5V→3.3V già previsto nel cablaggio? | → SÌ / NO |
| BME680 | Conferma definitiva: assente dal prototipo (firmware: flag a 0) | → confermato? |

### 1.3 Geometrie e direzioni (servono per le soglie firmware)

| Misura | Rilevato | Va in |
|---|---|---|
| Profondità interna tanica (fondo→coperchio) | → ____ cm | `tank <cm>` |
| Distanza US4→acqua con tanica **piena** | → ____ cm | taratura |
| Distanza US4→acqua con tanica **vuota** | → ____ cm | `tank <cm>` (≈ vuota − 2) |
| Forcella: ADC in **aria** / **acqua** / **terriccio umido** | → ____ / ____ / ____ | `soil_dry_threshold` |
| LDR: ADC **buio** / **ambiente** / **torcia** | → ____ / ____ / ____ | `light_threshold` |
| LDR: più luce = ADC più **alto** o più **basso**? | → ____ | verso light/shadow-seek |
| Motori: `motor f` → entrambe le ruote avanti? L/R corretti? | → ____ | mapping `Movement.cpp` |
| Relè: il canale resta diseccitato al boot? (attivo-basso confermato) | → SÌ / NO | `PUMP_RELAY_ACTIVE_LOW` |
| Altezza sonde di navigazione dal pavimento | → ____ cm | soglie ostacolo |

---

## 2. Schede componenti (documentazione e insidie)

### 2.1 HC-SR04 — sensore di distanza a ultrasuoni (×6)

**Principio**: un impulso di trigger di **10 µs** fa emettere un burst di
**8 cicli a 40 kHz**; il pin ECHO resta alto per un tempo proporzionale al
viaggio andata+ritorno del suono. Distanza ≈ `t_echo / 58` µs/cm.

**Specifiche chiave** (datasheet): alimentazione 5V, assorbimento ~15 mA,
portata 2–400 cm (ottimale 10–250 cm), risoluzione ~3 mm, **cono utile ~15°**,
frequenza massima di interrogazione ~20 Hz (≥ 50–60 ms tra letture della
stessa sonda).

**Insidie note**:
- **Zona cieca sotto i 2 cm** e letture instabili sotto i 4 cm.
- Superfici **morbide** (tessuto, fogliame) o **molto inclinate** assorbono o
  deviano l'eco → letture mancanti. Il pavimento del laboratorio è il caso
  facile; il vaso con la pianta no.
- **Cross-talk**: due sonde che ascoltano lo stesso eco si ingannano a
  vicenda. Il firmware legge **una sonda ogni 30 ms in round-robin**, quindi
  due letture consecutive distano 30 ms e la stessa sonda viene riletta ogni
  180 ms: i vincoli sono rispettati per costruzione.
- Su out-of-range alcuni cloni tengono ECHO alto fino a ~200 ms: il driver
  (`Ultrasonic.cpp`) usa un timeout e ritorna `NAN`, e la rilettura avviene
  ben oltre l'esaurimento dell'eco.

**Come lo tratta il firmware**: filtro EMA (α=0.4), range valido 2–200 cm
(120 per US4), 10 letture invalide consecutive → `NAN` → per US4 scatta il
fail-safe pompa.

**Da verificare domani**: vedi test T2/T3. Se una sonda legge sempre `NAN`:
trig/echo invertiti (confronta con `PINS - Sheet1.csv`) o VCC/GND assenti.

Fonti: [datasheet SparkFun](https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf) ·
[user guide Handsontec](https://www.handsontec.com/dataspecs/HC-SR04-Ultrasonic.pdf) ·
[guida Adafruit](https://cdn-learn.adafruit.com/downloads/pdf/ultrasonic-sonar-distance-sensors.pdf)

### 2.2 DS3232 — Real-Time Clock I²C

**Principio**: RTC con oscillatore **TCXO compensato in temperatura**:
precisione **±2 ppm** (≈ ±1 minuto/anno). Indirizzo I²C **0x68**. Il DS3232
aggiunge SRAM rispetto al DS3231; per noi sono equivalenti.

**Batteria tampone**: una **CR2032** mantiene l'ora a dispositivo spento
(assorbimento di backup ~3 µA → 3–10 anni di vita).

**Il flag che ci interessa — OSF** (Oscillator Stop Flag, bit 7 del registro
status 0x0F): vale 1 se l'oscillatore si è fermato dall'ultima impostazione
dell'ora → **l'ora letta non è affidabile**. Tipico di un modulo nuovo o con
batteria scarica/assente. Il firmware lo legge (`rtc` in CLI) e `rtc set
<epoch>` lo azzera scrivendo l'ora.

**Da verificare domani**: batteria presente? `rtc` → `time_valid`; dopo
`rtc set`, **togliere alimentazione 30 s** e ricontrollare che l'ora avanzi
(test della batteria tampone, T6).

Fonti: [datasheet DS3231 (Analog Devices)](https://www.analog.com/media/en/technical-documentation/data-sheets/ds3231.pdf) ·
[guida modulo](https://easyelecmodule.com/ultimate-ds3231-rtc-module/)

### 2.3 Forcella — sensore resistivo di umidità del suolo

**Principio**: due puntali misurano la **conducibilità** del terreno: più
acqua → meno resistenza → (nei moduli tipo YL-69/FC-28 con uscita analogica)
**tensione AO più bassa** → valore ADC più basso. Valori tipici: aria/secco
vicino a **1023**, immerso in acqua **~200–400**.

⚠️ **Attenzione alla direzione**: vale per la topologia standard dei moduli
commerciali. Se la nostra forcella è cablata su un partitore "fatto in casa"
la direzione potrebbe essere invertita → **va misurata, non assunta** (T4).
Il default firmware `soil_dry_threshold = 450` assume "secco = valore basso"
ed è probabilmente **da invertire o ricalibrare** dopo il test.

**Insidia nota — corrosione per elettrolisi**: con i puntali alimentati in
continua il metallo si corrode (degrado visibile in 6–12 mesi). Il firmware
oggi legge l'ADC in continuo con la forcella sempre alimentata a 5V:
**TODO futuro** — alimentare la forcella da un GPIO e accenderla solo per la
lettura (pochi ms ogni minuto), allungando la vita del sensore di anni.

**Da verificare domani**: T4 (tre valori: aria / acqua / terriccio umido) e
annotare la direzione. Controllare anche se c'è un modulo comparatore LM393
nel mezzo (in quel caso usare l'uscita **AO**, non DO).

Fonti: [Random Nerd Tutorials YL-69](https://randomnerdtutorials.com/guide-for-soil-moisture-sensor-yl-69-or-hl-69-with-the-arduino/) ·
[datasheet YL-69](https://www.electronicoscaldas.com/datasheet/YL-69-HL-69.pdf) ·
[guida FC-28](https://maker.pro/arduino/projects/arduino-soil-moisture-sensor)

### 2.4 Fotoresistore (LDR, presunto GL5528)

**Principio**: resistenza che cala con la luce. GL5528: **8–20 kΩ a 10 lux**,
**> 1 MΩ al buio**. Si legge con un partitore (tipico 10 kΩ fisso).

**Direzione del segnale** (da determinare, T5):
- LDR verso **VCC** (fisso verso GND): più luce → ADC **più alto** (topologia
  più comune, dà ~1.6–2.5 V in interni).
- LDR verso **GND**: più luce → ADC **più basso**.

Il default firmware `light_threshold = 600` e la logica light/shadow-seek
(`Movement.cpp`: gira a destra se `lux < soglia` in modalità LIGHT) assumono
"più luce = valore più alto". **Se il partitore è invertito, il robot
cercherà il buio credendo di cercare la luce**: per questo T5 è obbligatorio
prima di provare `mode light`.

Fonti: [specifiche GL5528](https://www.devobox.com/en/photosensors/9-photoresistor-gl5528-ldr.html) ·
[guida partitore LDR](https://blog.udemy.com/arduino-ldr/)

### 2.5 Driver motori (presunto L298N, doppio ponte H)

**Principio**: due ponti H pilotati da `IN1/IN2` (direzione motore A),
`IN3/IN4` (motore B) e `ENA/ENB` in PWM per la velocità. Logica 5V TTL.

**Specifiche e insidie**:
- **Caduta di tensione interna ~1.8–2.5 V** (fino a 4 V sotto forte carico):
  con alimentazione 12 V ai motori arrivano ~10 V; con 6 V ne arrivano ~4.
  Se i motori sembrano fiacchi a `calib 255 255`, la causa è quasi
  certamente questa → annotare Vs e la tensione effettiva ai morsetti (T7).
- **Jumper 5V del regolatore**: inserito = il modulo si genera i 5V interni
  da Vs (lecito solo con Vs ≤ 12 V) e il pin 5V diventa un'uscita. Con
  Vs > 12 V il jumper va tolto e i 5V vanno forniti dall'esterno.
- Pin del nostro cablaggio (CSV): ENA=D6, IN1=D43, IN2=D45 (sinistra);
  ENB=D7, IN3=D47, IN4=D49 (destra) — quale canale sia davvero la ruota
  sinistra è da verificare a banco (T7).

Fonti: [Last Minute Engineers L298N](https://lastminuteengineers.com/l298n-dc-stepper-driver-arduino-tutorial/) ·
[Components101](https://components101.com/modules/l293n-motor-driver-module) ·
[HowToMechatronics](https://howtomechatronics.com/tutorials/arduino/arduino-dc-motor-control-tutorial-l298n-pwm-h-bridge/)

### 2.6 Modulo relè 5V (pompa)

**Principio**: il GPIO pilota (di solito via optoaccoppiatore) la bobina di
un relè elettromeccanico (tipico Songle **SRD-05VDC-SL-C**, contatti fino a
10 A / 30 V DC). Il carico va sui contatti **COM + NO** (normally open):
relè a riposo = pompa spenta.

**Insidie**:
- **Polarità del trigger**: i moduli con optoaccoppiatore sono quasi sempre
  **attivi BASSI** (GPIO LOW = relè ON), quelli a transistor semplice spesso
  attivi alti. Il firmware assume attivo-basso (`PUMP_RELAY_ACTIVE_LOW 1` in
  `Pump.cpp`): il test T8 lo conferma o smentisce in 10 secondi.
- **Jumper JD-VCC**: se presente e rimosso, la bobina va alimentata a parte
  (isolamento galvanico). Con il jumper inserito bobina e logica condividono
  il 5V — va bene per noi, ma la bobina assorbe ~70 mA: contare anche questo
  nel budget del 5V.
- La bobina del relè ha già il suo diodo sul modulo, ma la **pompa** (motore
  DC) genera spike di back-EMF sui contatti: se non c'è un **diodo flyback
  (es. 1N4007) in antiparallelo sulla pompa**, aggiungerlo (catodo al +).

Fonti: [Circuit Basics relay](https://www.circuitbasics.com/setting-up-a-5v-relay-on-the-arduino/) ·
[datasheet SRD-05VDC](https://www.datasheetcafe.com/srd-05vdc-sl-c-datasheet-pdf/) ·
[JD-VCC spiegato](https://www.geya.net/5v-relay-module-how-it-works-and-application/)

### 2.7 Pompa sommersa mini (presunta 3–6V DC)

**Specifiche tipiche**: 3–6 V, 100–200 mA, prevalenza ~40–110 cm. Non va mai
pilotata da un GPIO (limite ~40 mA): da noi passa dal relè.

**Insidie**:
- **Marcia a secco**: le pompette sommerse usano l'acqua per raffreddarsi e
  lubrificarsi: a secco si rovinano in fretta. È esattamente il motivo della
  **protezione tanica** implementata nel firmware (blocco su `tank_empty_cm`
  + auto-stop): il test T9 la valida end-to-end.
- **Diodo flyback** sulla pompa se assente (vedi 2.6).
- Adescamento: alla prima accensione può servire qualche secondo perché
  l'acqua riempia la girante → fare il primo test con `pump 5000`.

Fonti: [guida pompe mini](https://zbotic.in/mini-water-pump-guide-dc-submersible-peristaltic-pumps-for-arduino/) ·
[controllo pompa con relè](https://www.hibit.dev/posts/128/how-to-control-mini-water-pump-with-arduino)

### 2.8 ESP32-CAM (AI-Thinker, OV2640)

**Alimentazione — la causa n.1 dei problemi**: serve **5V con almeno
500 mA reali** (raccomandati 2 A); con Wi-Fi + camera i picchi causano
**brownout** (reset spontanei) se l'alimentazione è debole (es. presa dal
FTDI). Rimedio: alimentazione 5V dedicata + **condensatore elettrolitico
≥ 470 µF tra 5V e GND** vicino alla scheda. Il firmware disabilita il
brownout-detector al boot, ma è un cerotto: se T11 mostra reset, è
l'alimentazione.

**Flash del firmware**: senza base MB serve un FTDI a 3.3V e **GPIO0 a GND
durante il reset** per entrare in bootloader; poi scollegare GPIO0 e
resettare. GPIO4 pilota il LED flash di bordo (oggi non usato dal firmware).

**Camera**: il flat cable della OV2640 è la causa n.1 di `Camera init
FAILED` — riseatarlo con delicatezza.

Fonti: [pinout Random Nerd Tutorials](https://randomnerdtutorials.com/esp32-cam-ai-thinker-pinout/) ·
[fix brownout](https://www.makerguides.com/fix-brownout-of-esp32-cam/) ·
[guida sviluppo DroneBot](https://dronebotworkshop.com/esp32-cam-develop/)

---

## 3. Checklist test pratici (robot cablato)

> Prerequisito: firmware flashati (v5.1 / v1.2 / v2.1), monitor seriale a
> 115200. Sul Mega: **`standalone on`** come primo comando.
> Procedura dettagliata dei passi in `Lab_Bringup_Checklist.md`.

### T1 — Alimentazione e masse
- [ ] Multimetro: 5V del Mega sotto carico → atteso 4.75–5.25 V. Rilevato → ____ V
- [ ] Vs motori all'ingresso H-bridge → ____ V (compila §1.1)
- [ ] Continuità GND fra Mega, H-bridge, relè, sensori → atteso < 1 Ω ovunque
- [ ] Nessun reset spontaneo del Mega in 5 minuti di `status` ripetuti

### T2 — Ultrasuoni di navigazione (US1, US2, US3, US5, US6)
Per ciascuna sonda, bersaglio rigido (libro/cartone) e metro alla mano,
leggere `sensors`:
- [ ] a **10 cm** → rilevato: US1 ____ US2 ____ US3 ____ US5 ____ US6 ____
- [ ] a **50 cm** → rilevato: US1 ____ US2 ____ US3 ____ US5 ____ US6 ____
- [ ] a **100 cm** → rilevato: US1 ____ US2 ____ US3 ____ US5 ____ US6 ____
- Esito atteso: errore < ±2 cm fino a 1 m. Mano davanti alla sonda → il
  valore scende in ~1 s (assestamento EMA).

### T3 — US4 / tanica
- [ ] Tanica piena → `tank`: rilevato ____ cm
- [ ] Tanica vuota → `tank`: rilevato ____ cm
- [ ] Soglia impostata: `tank ____` (≈ vuota − 2) e verifica con `config`

### T4 — Forcella umidità (annota la DIREZIONE)
- [ ] In **aria** → `sensors`: soil = ____
- [ ] In **acqua** → soil = ____
- [ ] In **terriccio umido** → soil = ____
- Direzione: bagnato = valore più **alto / basso**? → ____
  (se "bagnato = basso" il default `soil_dry_threshold=450` va ripensato)

### T5 — Fotoresistore (PRIMA di provare mode light/shadow)
- [ ] LDR **coperto** → lux = ____
- [ ] Luce **ambiente** → lux = ____
- [ ] **Torcia** addosso → lux = ____
- Direzione: più luce = valore più **alto / basso**? → ____
  (il firmware assume "alto"; se invertito, light-seek e shadow-seek
  risultano scambiati)

### T6 — RTC
- [ ] `rtc` → rtc_ok = YES; time_valid = ____
- [ ] `rtc set <epoch>` (epoch da PowerShell: `[DateTimeOffset]::Now.ToUnixTimeSeconds()`)
- [ ] Spegnere il Mega 30 s, riaccendere → `rtc`: l'ora è avanzata? → SÌ / NO
      (NO = batteria CR2032 scarica/assente)

### T7 — Motori (ruote sollevate, poi a terra)
- [ ] `motor f 1000` → entrambe avanti? → ____ (se una è invertita: fili del motore)
- [ ] `motor l` / `motor r` → coerenti con sinistra/destra fisica? → ____
      (se scambiati: scambiare i gruppi LEFT/RIGHT in `Movement.cpp`)
- [ ] Tensione ai morsetti motore con `calib 255 255` in `motor f` → ____ V
      (attesa: Vs − ~2 V per la caduta dell'H-bridge)
- [ ] A terra, 2 m di rettilineo con `mode light` in ambiente uniforme o
      `motor f 3000`: deriva laterale → ____ cm → correggere con `calib`

### T8 — Relè (pompa SCOLLEGATA)
- [ ] Al power-on il relè NON scatta → SÌ / NO
      (NO = modulo attivo-alto → `PUMP_RELAY_ACTIVE_LOW 0` e riflash)
- [ ] Con US4 su bersaglio < soglia: `pump 1000` → click ON e click OFF dopo 1 s
- [ ] Multimetro su COM/NO: contatto chiuso solo durante il comando

### T9 — Pompa + protezione tanica (il test della richiesta v5.1)
- [ ] Tanica vuota: `pump 2000` → atteso `BLOCCATO: tanica vuota` → OK? ____
- [ ] US4 scollegato (o coperto): `pump 2000` → atteso `tank_sensor_fault` → ____
- [ ] Tanica piena: `pump 5000` → la pompa gira e l'acqua scorre → ____
- [ ] Portata: ml raccolti in 10 s di `pump 10000` → ____ ml
      (serve per dimensionare i futuri `water` automatici)
- [ ] **Auto-stop**: `pump 30000` e sollevare US4/svuotare → la pompa si
      ferma da sola con log `pump_stop` → OK? ____

### T10 — Catena Hub ↔ Mega (con partitore sul TX del Mega!)
- [ ] Hub `status` → mega_link OK entro 10 s dal collegamento
- [ ] Hub `telemetry` → distanze coerenti con `sensors` sul Mega
- [ ] Hub `soil` → `[ACK Mega] ... value=` uguale al T4
- [ ] Hub `water 2000` con tanica piena → ACK OK + pompa attiva
- [ ] Hub `water 2000` con tanica vuota → ACK `ERROR tank_empty`
- [ ] Mega `standalone off` → nessun degraded con Hub collegato (heartbeat 30 s)
- [ ] Hub spento 2+ min → Mega in degraded `hub_missing`; riacceso → recovery

### T11 — ESP32-CAM
- [ ] `status` → camera = OK; free_heap → ____ B
- [ ] `capture` ×5 → 5 frame ok, dimensioni ____ ÷ ____ byte, tempo ____ ms
- [ ] Nessun reset/brownout durante le catture → OK? ____
      (se resetta: alimentazione, vedi scheda 2.8)

### T12 — Stress finale (se resta tempo)
- [ ] Mega: 10 minuti in `mode light` in area con ostacoli: il robot evita,
      non entra mai in STUCK permanente, `stats` a fine giro → annotare
      obstacles_avoided ____ , stuck_events ____ , watchdog_resets ____ (atteso 0)
- [ ] `status` a fine stress: freeRam → ____ B (atteso stabile ≳ 3500)

---

## 4. A fine giornata

Riportare i valori raccolti qui e in `SmartVase_Project_State.md`, e se
servono modifiche firmware (polarità relè, direzione LDR/forcella, swap
motori) committarle subito con i valori misurati nel messaggio di commit:
fra una settimana questi numeri saranno l'unica memoria affidabile.
