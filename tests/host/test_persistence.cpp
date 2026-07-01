// =====================================================================
// Unit test NATIVO (host, g++) del modulo Persistence.cpp del Mega.
// Testa la logica del sistema dual-slot per configurazione e statistiche.
//
// In particolare valida:
//   - Il caricamento dei default di fabbrica a EEPROM vuota.
//   - L'alternanza dei salvataggi tra Slot 0 e Slot 1.
//   - L'uso di write_counter per caricare lo slot più recente al boot.
//   - Il fallback sullo slot meno recente ma valido se il più recente è corrotto.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cassert>

#include "EEPROM.h"

// Definisce l'oggetto EEPROM globale stubbato
EEPROMClass EEPROM;

// Mock del tempo Arduino
static unsigned long g_mock_millis = 0;
unsigned long millis() {
    return g_mock_millis;
}

// Inclusione del codice reale del Mega
#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Crc16.cpp"
#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/Persistence.cpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

int main() {
    printf("== test_persistence ==\n");

    // =================================================================
    // 1) Test caricamento default su EEPROM vuota
    // =================================================================
    {
        Persistence p;
        p.loadConfig();

        CHECK(p.getConfig().magic_number == EEPROM_MAGIC_NUMBER_CONFIG, "Default config magic number corretto");
        CHECK(p.getConfig().write_counter == 0, "Default config write_counter e' 0");
        CHECK(p.getConfig().avoid_reverse_ms == 1000, "Default avoid_reverse_ms e' 1000");
        CHECK(p.getConfig().tank_empty_cm == 20, "Default tank_empty_cm e' 20");
    }

    // =================================================================
    // 2) Test alternanza dei salvataggi (Slot 0 e Slot 1)
    // =================================================================
    {
        Persistence p;
        p.loadConfig(); // inizializza e scrive Slot 0 (counter=0)

        // Modifica e salva -> deve andare su Slot 1 con counter=1
        p.getConfig().avoid_reverse_ms = 2000;
        p.saveConfig(true);

        DeviceConfig c0, c1;
        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        CHECK(c1.avoid_reverse_ms == 2000, "Modifica scritta nello Slot 1");
        CHECK(c1.write_counter == 1, "Slot 1 ha counter == 1");
        CHECK(c0.avoid_reverse_ms == 1000, "Slot 0 ha mantenuto vecchio valore (1000)");

        // Altra modifica e salva -> deve andare su Slot 0 con counter=2
        p.getConfig().avoid_reverse_ms = 3000;
        p.saveConfig(true);

        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        CHECK(c0.avoid_reverse_ms == 3000, "Modifica scritta nello Slot 0");
        CHECK(c0.write_counter == 2, "Slot 0 ha counter == 2");
        CHECK(c1.avoid_reverse_ms == 2000, "Slot 1 ha mantenuto il valore precedente (2000)");
    }

    // =================================================================
    // 3) Test caricamento dello slot più recente al boot (reboot)
    // =================================================================
    {
        Persistence p2;
        p2.loadConfig();

        CHECK(p2.getConfig().avoid_reverse_ms == 3000, "Reboot caricato correttamente Slot 0 (piu' recente, counter=2)");
    }

    // =================================================================
    // 4) Test fallback su slot valido se il più recente è corrotto
    // =================================================================
    {
        // Corrompiamo lo Slot 0 (impostando magic_number a zero)
        DeviceConfig c0;
        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        c0.magic_number = 0; // invalida lo slot 0
        EEPROM.put(EEPROM_CONFIG_SLOT_0_ADDR, c0);

        Persistence p3;
        p3.loadConfig();

        CHECK(p3.getConfig().avoid_reverse_ms == 2000, "Fallback su Slot 1 (counter=1) riuscito perche' Slot 0 corrotto");
    }

    // =================================================================
    // 5) Test caricamento default se entrambi gli slot sono corrotti
    // =================================================================
    {
        // Invalida anche lo Slot 1
        DeviceConfig c1;
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
        c1.magic_number = 0;
        EEPROM.put(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        Persistence p4;
        p4.loadConfig();

        CHECK(p4.getConfig().avoid_reverse_ms == 1000, "Caricato default perche' entrambi gli slot corrotti");
    }

    // =================================================================
    // 6) Test CumulativeStats wear leveling
    // =================================================================
    {
        Persistence p;
        p.loadStats(); // Inizializza statistiche a zero (counter=0, Slot 0)

        CHECK(p.getStats().magic_number == EEPROM_MAGIC_NUMBER_STATS, "Stats initialized with correct magic number");
        CHECK(p.getStats().write_counter == 0, "Initial stats write_counter is 0");

        // Incrementa contatore e salva -> deve andare a Slot 1 con counter=1
        p.getStats().obstacles_avoided = 5;
        p.saveStats(true);

        CumulativeStats s0, s1;
        EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
        EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);

        CHECK(s1.obstacles_avoided == 5, "Stats modificate salvate su Slot 1");
        CHECK(s1.write_counter == 1, "Slot 1 stats counter == 1");

        // Altro salvataggio -> Slot 0 con counter=2
        p.getStats().obstacles_avoided = 10;
        p.saveStats(true);

        EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
        CHECK(s0.obstacles_avoided == 10, "Stats modificate salvate su Slot 0");
        CHECK(s0.write_counter == 2, "Slot 0 stats counter == 2");

        // Simula reboot
        Persistence p2;
        p2.loadStats();
        CHECK(p2.getStats().obstacles_avoided == 10, "Uptime caricato correttamente Slot 0 (piu' recente)");

        // Corrompiamo lo Slot 0
        s0.magic_number = 0;
        EEPROM.put(EEPROM_STATS_SLOT_0_ADDR, s0);

        Persistence p3;
        p3.loadStats();
        CHECK(p3.getStats().obstacles_avoided == 5, "Fallback su Slot 1 stats riuscito");
    }

    // =================================================================
    // 7) Throttle config: saveConfig(false) non scrive entro l'intervallo
    //    (anti-usura EEPROM); scrive una volta passato l'intervallo.
    // =================================================================
    {
        g_mock_millis = 0;
        Persistence p;
        p.loadConfig();
        p.saveConfig(true);                       // forza una scrittura: lastWrite=0
        uint16_t wc = p.getConfig().write_counter;

        g_mock_millis = 30000;                    // 30 s < 60 s
        p.getConfig().avoid_turn_ms = 999;
        p.saveConfig(false);                       // dentro la finestra -> NON scrive
        CHECK(p.getConfig().write_counter == wc, "saveConfig(false) entro 60s: nessuna scrittura");

        g_mock_millis = 70000;                    // > 60 s dall'ultima scrittura
        p.saveConfig(false);                       // fuori finestra -> scrive
        CHECK(p.getConfig().write_counter == (uint16_t)(wc + 1), "saveConfig(false) dopo 60s: scrive");
    }

    // =================================================================
    // 8) No-op stats: saveStats(false) non scrive se i dati non sono cambiati
    //    (memcmp), scrive dopo una modifica reale.
    // =================================================================
    {
        g_mock_millis = 0;
        Persistence p;
        p.loadStats();
        p.getStats().stuck_events = 7;
        p.saveStats(true);                         // scrive: lastSavedStats allineato
        uint16_t wc = p.getStats().write_counter;

        g_mock_millis = 500000;                   // oltre l'intervallo stats (300 s)
        p.saveStats(false);                        // invariato -> memcmp uguale -> NON scrive
        CHECK(p.getStats().write_counter == wc, "saveStats(false) invariato: nessuna scrittura");

        p.getStats().stuck_events = 8;             // modifica reale
        p.saveStats(false);                        // cambiato + fuori finestra -> scrive
        CHECK(p.getStats().write_counter == (uint16_t)(wc + 1), "saveStats(false) dopo modifica: scrive");
    }

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
