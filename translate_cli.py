import os

filepath = 'firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/src/HubCli.cpp'
with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

replacements = {
    'versione firmware Hub mostrata dalla CLI (allineata a MainLogic.cpp)': 'Hub firmware version shown by CLI (aligned with MainLogic.cpp)',
    '[CLI] pronta: digita \\\'help\\\'': '[CLI] ready: type \\\'help\\\'',
    '\\n[CLI] riga troppo lunga, scartata': '\\n[CLI] line too long, discarded',
    '[CLI] config salvata su NVS (riavvia con \\\'reboot\\\')': '[CLI] config saved to NVS (restart with \\\'reboot\\\')',
    '[CLI] ERRORE salvataggio NVS': '[CLI] NVS save ERROR',
    '[CLI] riavvio...': '[CLI] rebooting...',
    '<valore>': '<value>',
    '[CLI] comando sconosciuto: \\\'': '[CLI] unknown command: \\\'',
    '\\\' (prova \\\'help\\\')': '\\\' (try \\\'help\\\')',
    '[CLI] porta non valida': '[CLI] invalid port',
    ' impostato (ricorda \\\'save\\\' + \\\'reboot\\\')': ' set (remember \\\'save\\\' + \\\'reboot\\\')',
    'comandi senza payload': 'commands without payload',
    '[CLI] comando inviato al Mega (cmd_id=': '[CLI] command sent to Mega (cmd_id=',
    '), attendi [ACK Mega]': '), wait for [ACK Mega]',
    '[CLI] ERRORE: coda TX verso il Mega piena': '[CLI] ERROR: TX queue to Mega is full',
    '(vuoto)': '(empty)',
    'CONNESSO  ip=': 'CONNECTED  ip=',
    'NON CONFIGURATO': 'NOT CONFIGURED',
    'CONNESSO"': 'CONNECTED"',
    'DISCONNESSO"': 'DISCONNECTED"',
    'OK (ultimo msg ': 'OK (last msg ',
    ' s fa)': ' s ago)',
    'ASSENTE (nessun messaggio dal Mega)': 'MISSING (no message from Mega)',
    '============== DIAGNOSTICA Hub ==============': '============== Hub DIAGNOSTICS ==============',
    'CONNESSO ip=': 'CONNECTED ip=',
    'verifica SSID/pass con \\\'show\\\'; hotspot acceso e a 2.4 GHz; poi \\\'wifi connect\\\'': 'check SSID/pass with \\\'show\\\'; hotspot on and at 2.4 GHz; then \\\'wifi connect\\\'',
    '--- NTP / ora (serve alla TLS) ---': '--- NTP / time (needed for TLS) ---',
    '[ok ora valida]': '[ok valid time]',
    'ora NON sincronizzata -> la TLS verso HiveMQ fallisce; serve internet sull\\\'hotspot': 'time NOT synced -> TLS to HiveMQ will fail; needs internet on the hotspot',
    'NON CONFIGURATO  [imposta \\\'set mqtt_broker/user/pass\\\' + \\\'save\\\' + \\\'reboot\\\']': 'NOT CONFIGURED  [set \\\'set mqtt_broker/user/pass\\\' + \\\'save\\\' + \\\'reboot\\\']',
    'CONNESSO ': 'CONNECTED ',
    'DISCONNESSO ': 'DISCONNECTED ',
    'WiFi ok ma MQTT giu\\\' -> ora/TLS/credenziali: controlla [NTP] sopra, user/pass, URL broker': 'WiFi ok but MQTT down -> time/TLS/credentials: check [NTP] above, user/pass, broker URL',
    'in attesa del Wi-Fi': 'waiting for Wi-Fi',
    '--- Link seriale col Mega ---': '--- Serial link with Mega ---',
    'ASSENTE  [!! nessun frame dal Mega]': 'MISSING  [!! no frame from Mega]',
    'cablaggio: Mega TX1(D18)->partitore->RX2(GPIO16), Hub TX2(GPIO17)->RX1(D19), GND comune': 'wiring: Mega TX1(D18)->divider->RX2(GPIO16), Hub TX2(GPIO17)->RX1(D19), common GND',
    '--- Sistema ---': '--- System ---',
    '[CLI] nessuna telemetria ricevuta dal Mega finora': '[CLI] no telemetry received from Mega so far',
    '--- ultima telemetria dal Mega ---': '--- last telemetry from Mega ---',
    'questo menu': 'this menu',
    'versione firmware': 'firmware version',
    'diagnostica Wi-Fi/MQTT/Mega con hint': 'Wi-Fi/MQTT/Mega diagnostics with hints',
    'configurazione NVS': 'NVS configuration',
    '<chiave>': '<key>',
    'salva config su NVS': 'save config to NVS',
    'ritenta connessione Wi-Fi': 'retry Wi-Fi connection',
    'riavvia l\\\'Hub': 'reboot the Hub',
    'ultima telemetria dal Mega': 'last telemetry from Mega',
    'irrigazione (protez. tanica)': 'watering (tank protection)',
    'modalita\\\' movimento': 'movement mode',
    'ferma motori e pompa': 'stop motors and pump',
    'umidita\\\' suolo': 'soil moisture',
    'chiede una TelemetryDeep al Mega': 'requests a TelemetryDeep from Mega',
    'soft reset del Mega': 'soft reset of Mega'
}

for k, v in replacements.items():
    content = content.replace(k, v)

with open(filepath, 'w', encoding='utf-8') as f:
    f.write(content)
print("done translating HubCli.cpp")
