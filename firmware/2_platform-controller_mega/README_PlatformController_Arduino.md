SmartVase - Firmware Platform Controller (Arduino Mega)

Questo repository contiene il firmware per il Platform Controller (basato su Arduino Mega) del progetto SmartVase, un sistema IoT per una serra automatizzata e mobile.

Questo firmware è responsabile della gestione a basso livello di tutto l'hardware, inclusi motori, sensori e attuatori. È stato progettato con un'enfasi su robustezza, resilienza e diagnostica avanzata, per un'operatività di livello "enterprise-ready".

Architettura del Sistema

Il sistema SmartVase è composto da tre microcontrollori che collaborano:

Arduino Mega (Questo Firmware) - Platform Controller: Il "braccio" del sistema. Gestisce direttamente tutto l'hardware (motori, pompa, sensori a ultrasuoni, sensore BME680, etc.). Comunica esclusivamente con l'Hub.

ESP32 Standard - Logic & Web Hub: Il "cervello". Gestisce la connettività Wi-Fi, un server web con API REST per l'app Android e coordina le azioni.

ESP32-CAM - Vision Co-Processor: L'"occhio". Gestisce la cattura e l'analisi base delle immagini.

Funzionalità Chiave del Firmware

Codice Non Bloccante: Tutta la logica è basata su millis() e macchine a stati. Non ci sono delay().

Watchdog Hardware (WDT): Reset automatico in caso di stallo software, con conteggio e logging dei reset.

Gestione Memoria Ottimizzata: Nessun uso della classe String e monitoraggio attivo della SRAM libera con ingresso in modalità di funzionamento degradata in caso di criticità.

Persistenza su EEPROM: Configurazioni e statistiche cumulative sono salvate in EEPROM con wear leveling (doppio slot) e convalida tramite CRC16.

Protocollo di Comunicazione Robusto: Comunicazione seriale con l'Hub tramite messaggi Protobuf incapsulati in un frame con Start-of-Frame, lunghezza e CRC16 per garantire l'integrità dei dati.

Diagnostica Avanzata:

Logging Strutturato: Tutti gli eventi importanti vengono loggati e inviati all'Hub.

CLI di Debug: Interfaccia a riga di comando su porta USB per test manuali e diagnostica sul campo senza bisogno dell'Hub.

Fail-Safe per Sensori: Monitoraggio dei fallimenti consecutivi dei sensori e gestione di dati inaffidabili.

Setup e Compilazione

Per compilare questo firmware, segui attentamente questi passaggi.

1. Dipendenze Hardware

<!-- Descrivi qui l'hardware necessario: Arduino Mega, motori, sensori, etc. -->

Arduino Mega 2560

...

2. Librerie Arduino

Installa le seguenti librerie tramite il Library Manager dell'IDE di Arduino:

Adafruit BME680 Library

HCSR04 by gamegine

DS3232RTC

3. Configurazione di Nanopb (Cruciale)

Questo progetto usa Nanopb per la serializzazione dei dati con Protobuf. Il setup è manuale.

Copia i File Core di Nanopb:

Trova la cartella della libreria nanopb di Arduino.

Copia i 7 file (pb.h, pb_common.c, pb_common.h, pb_decode.c, pb_decode.h, pb_encode.c, pb_encode.h) nella cartella principale di questo sketch.

Genera i File dello Sketch:

Installa Python e nanopb (pip install nanopb).

Esegui il generatore sul file smartvase_v3.proto per creare smartvase.pb.c and smartvase.pb.h.

Posiziona i file generati nella cartella di questo sketch.

Modifica Manuale (Importante!):

Apri il file smartvase.pb.h appena generato.

Trova la riga #include <pb.h>.

Modificala in #include "pb.h".

Salva il file.

4. Compila e Carica

Dopo aver seguito i passaggi precedenti, il progetto compilerà e potrà essere caricato sull'Arduino Mega.

Utilizzo della CLI di Debug

Collega l'Arduino Mega al PC via USB e apri il Serial Monitor a 115200 baud con terminatore di riga "Newline". Digita help per la lista dei comandi disponibili.

--- SmartVase CLI ---
help             - Mostra questo menu
status           - Mostra stato operativo
stats            - Mostra statistiche cumulative
config           - Mostra configurazione attuale
sensors          - Mostra valori sensori
reboot           - Riavvia il microcontrollore
motor <dir> <ms> - Test motori (dir: f,b,l,r)
pump <ms>        - Test pompa


Roadmap Futura

[ ] Integrazione di un sensore di corrente (es. INA219) per il rilevamento di stallo dei motori.

[ ] Implementazione di un sistema di aggiornamento firmware Over-The-Air (OTA) gestito dall'Hub.

Licenza

Questo progetto è rilasciato sotto la Licenza MIT. Vedi il file LICENSE per maggiori dettagli.
