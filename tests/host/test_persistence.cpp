// =====================================================================
// NATIVE unit test (host, g++) of the Mega's Persistence.cpp module.
// Tests the logic of the dual-slot system for config and stats.
//
// Specifically validates:
//   - Loading factory defaults on empty EEPROM.
//   - Alternating saves between Slot 0 and Slot 1.
//   - Use of write_counter to load the most recent slot at boot.
//   - Fallback to the older but valid slot if the newer one is corrupted.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cassert>

#include "EEPROM.h"

// Defines the stubbed global EEPROM object
EEPROMClass EEPROM;

// Mock of Arduino time
static unsigned long g_mock_millis = 0;
unsigned long millis() {
    return g_mock_millis;
}

// Inclusion of real Mega code
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
    // 1) Test loading defaults on empty EEPROM
    // =================================================================
    {
        Persistence p;
        p.loadConfig();

        CHECK(p.getConfig().magic_number == EEPROM_MAGIC_NUMBER_CONFIG, "Default config magic number correct");
        CHECK(p.getConfig().write_counter == 0, "Default config write_counter is 0");
        CHECK(p.getConfig().avoid_reverse_ms == 1000, "Default avoid_reverse_ms is 1000");
        CHECK(p.getConfig().tank_empty_cm == 20, "Default tank_empty_cm is 20");
    }

    // =================================================================
    // 2) Test alternating saves (Slot 0 and Slot 1)
    // =================================================================
    {
        Persistence p;
        p.loadConfig(); // initializes and writes Slot 0 (counter=0)

        // Modify and save -> should go to Slot 1 with counter=1
        p.getConfig().avoid_reverse_ms = 2000;
        p.saveConfig(true);

        DeviceConfig c0, c1;
        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        CHECK(c1.avoid_reverse_ms == 2000, "Modification written to Slot 1");
        CHECK(c1.write_counter == 1, "Slot 1 has counter == 1");
        CHECK(c0.avoid_reverse_ms == 1000, "Slot 0 kept old value (1000)");

        // Another modify and save -> should go to Slot 0 with counter=2
        p.getConfig().avoid_reverse_ms = 3000;
        p.saveConfig(true);

        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        CHECK(c0.avoid_reverse_ms == 3000, "Modification written to Slot 0");
        CHECK(c0.write_counter == 2, "Slot 0 has counter == 2");
        CHECK(c1.avoid_reverse_ms == 2000, "Slot 1 kept previous value (2000)");
    }

    // =================================================================
    // 3) Test loading the most recent slot at boot (reboot)
    // =================================================================
    {
        Persistence p2;
        p2.loadConfig();

        CHECK(p2.getConfig().avoid_reverse_ms == 3000, "Reboot correctly loaded Slot 0 (most recent, counter=2)");
    }

    // =================================================================
    // 4) Test fallback to valid slot if the most recent is corrupted
    // =================================================================
    {
        // Corrupt Slot 0 (setting magic_number to zero)
        DeviceConfig c0;
        EEPROM.get(EEPROM_CONFIG_SLOT_0_ADDR, c0);
        c0.magic_number = 0; // invalidate slot 0
        EEPROM.put(EEPROM_CONFIG_SLOT_0_ADDR, c0);

        Persistence p3;
        p3.loadConfig();

        CHECK(p3.getConfig().avoid_reverse_ms == 2000, "Fallback to Slot 1 (counter=1) succeeded because Slot 0 corrupted");
    }

    // =================================================================
    // 5) Test loading defaults if both slots are corrupted
    // =================================================================
    {
        // Invalidate Slot 1 as well
        DeviceConfig c1;
        EEPROM.get(EEPROM_CONFIG_SLOT_1_ADDR, c1);
        c1.magic_number = 0;
        EEPROM.put(EEPROM_CONFIG_SLOT_1_ADDR, c1);

        Persistence p4;
        p4.loadConfig();

        CHECK(p4.getConfig().avoid_reverse_ms == 1000, "Loaded default because both slots corrupted");
    }

    // =================================================================
    // 6) Test CumulativeStats wear leveling
    // =================================================================
    {
        Persistence p;
        p.loadStats(); // Initializes stats to zero (counter=0, Slot 0)

        CHECK(p.getStats().magic_number == EEPROM_MAGIC_NUMBER_STATS, "Stats initialized with correct magic number");
        CHECK(p.getStats().write_counter == 0, "Initial stats write_counter is 0");

        // Increment counter and save -> should go to Slot 1 with counter=1
        p.getStats().obstacles_avoided = 5;
        p.saveStats(true);

        CumulativeStats s0, s1;
        EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
        EEPROM.get(EEPROM_STATS_SLOT_1_ADDR, s1);

        CHECK(s1.obstacles_avoided == 5, "Modified stats saved to Slot 1");
        CHECK(s1.write_counter == 1, "Slot 1 stats counter == 1");

        // Another save -> Slot 0 with counter=2
        p.getStats().obstacles_avoided = 10;
        p.saveStats(true);

        EEPROM.get(EEPROM_STATS_SLOT_0_ADDR, s0);
        CHECK(s0.obstacles_avoided == 10, "Modified stats saved to Slot 0");
        CHECK(s0.write_counter == 2, "Slot 0 stats counter == 2");

        // Simulate reboot
        Persistence p2;
        p2.loadStats();
        CHECK(p2.getStats().obstacles_avoided == 10, "Loaded correctly from Slot 0 (most recent)");

        // Corrupt Slot 0
        s0.magic_number = 0;
        EEPROM.put(EEPROM_STATS_SLOT_0_ADDR, s0);

        Persistence p3;
        p3.loadStats();
        CHECK(p3.getStats().obstacles_avoided == 5, "Fallback to Slot 1 stats succeeded");
    }

    // =================================================================
    // 7) Throttle config: saveConfig(false) does not write within interval
    //    (EEPROM wear-leveling); writes once interval passed.
    // =================================================================
    {
        g_mock_millis = 0;
        Persistence p;
        p.loadConfig();
        p.saveConfig(true);                       // force a write: lastWrite=0
        uint16_t wc = p.getConfig().write_counter;

        g_mock_millis = 30000;                    // 30 s < 60 s
        p.getConfig().avoid_turn_ms = 999;
        p.saveConfig(false);                       // inside window -> DOES NOT write
        CHECK(p.getConfig().write_counter == wc, "saveConfig(false) within 60s: no write");

        g_mock_millis = 70000;                    // > 60 s since last write
        p.saveConfig(false);                       // outside window -> writes
        CHECK(p.getConfig().write_counter == (uint16_t)(wc + 1), "saveConfig(false) after 60s: writes");
    }

    // =================================================================
    // 8) No-op stats: saveStats(false) does not write if data hasn't changed
    //    (memcmp), writes after a real modification.
    // =================================================================
    {
        g_mock_millis = 0;
        Persistence p;
        p.loadStats();
        p.getStats().stuck_events = 7;
        p.saveStats(true);                         // writes: lastSavedStats aligned
        uint16_t wc = p.getStats().write_counter;

        g_mock_millis = 500000;                   // beyond stats interval (300 s)
        p.saveStats(false);                        // unchanged -> memcmp equals -> DOES NOT write
        CHECK(p.getStats().write_counter == wc, "saveStats(false) unchanged: no write");

        p.getStats().stuck_events = 8;             // real modification
        p.saveStats(false);                        // changed + outside window -> writes
        CHECK(p.getStats().write_counter == (uint16_t)(wc + 1), "saveStats(false) after modification: writes");
    }

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
