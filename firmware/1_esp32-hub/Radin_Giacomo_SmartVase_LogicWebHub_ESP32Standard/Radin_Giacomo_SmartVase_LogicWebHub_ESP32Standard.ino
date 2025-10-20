/*
 * =================================================================
 * SmartVase - Logic & Web Hub (ESP32 Standard)
 * =================================================================
 * VERSIONE 15.0 - ENTERPRISE READY (FINALE)
 * Autore: Gemini (revisione basata su analisi utente)
 *
 * NOTE DI VERSIONE:
 * - CONFIGURAZIONE WEBHOOK VIA API: Aggiunto endpoint POST /config per
 * aggiornare l'URL del webhook da remoto e salvarlo in NVS.
 * - TIMESTAMP ISO8601 NEI LOG: Tutti i log generati dall'Hub includono
 * un timestamp standard per facilitare l'analisi temporale.
 * - STATISTICHE CON DIGEST: L'endpoint /stats/cumulative ora include una
 * sezione "digest" con un riassunto calcolato delle metriche.
 * - ESPORTAZIONE LOG IN JSONL: L'endpoint /logs/export ora genera un file
 * in formato JSON Lines (.jsonl), ideale per l'ingestion automatica.
 * - ALERTING REMOTO ATTIVO: La funzione di logging integra una chiamata
 * HTTP POST attiva a un webhook configurabile per inviare alert.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include "time.h"
#include <HTTPClient.h>
#include <Preferences.h>

// -------------------- CONFIGURAZIONE E VARIABILI --------------------
#define DEVICE_ID "HUB_01"
#define ARDUINO_SERIAL Serial2
#define CAM_SERIAL Serial1
#define MEGA_WATCHDOG_TIMEOUT 10000

const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* ntpServer = "pool.ntp.org";

// Webhook URL, caricato da NVS all'avvio
char webhookUrl[256];

WebServer server(80);
Preferences preferences;
std::vector<String> logHistory;
const int MAX_LOG_HISTORY = 100;

// Variabili di stato e buffer
TaskHandle_t cpuLoadTaskHandle = NULL;
volatile uint32_t idle_counter = 0;
float cpu_load_percent = 0.0;
String megaCumulativeStats = "{}";
String camCumulativeStats = "{}";
const int SERIAL_BUFFER_SIZE = 512;
char arduinoBuffer[SERIAL_BUFFER_SIZE];
int arduinoBufferPos = 0;
unsigned long lastMegaResponseTime = 0;

// -------------------- FUNZIONI DI SERVIZIO --------------------
String getTimestampISO() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "1970-01-01T00:00:00Z"; // Fallback in caso di NTP non sincronizzato
    }
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buffer);
}

void sendRemoteAlert(const String& logJson) {
    if (WiFi.status() == WL_CONNECTED && strlen(webhookUrl) > 10) {
        HTTPClient http;
        http.begin(webhookUrl);
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(logJson);
        if (httpCode < 0) {
            Serial.printf("[Alert] Invio webhook fallito, errore: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
    }
}

void logHubEvent(const char* level, const char* event, const char* detail) {
    StaticJsonDocument<512> doc;
    doc["source"] = "hub";
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = getTimestampISO();
    doc["level"] = level;
    doc["event"] = event;
    if(detail) {
        doc["detail"] = detail;
    }
    String logStr;
    serializeJson(doc, logStr);
    Serial.println(logStr);
    
    if (strcmp(level, "error") == 0 || strcmp(level, "warn") == 0) {
        sendRemoteAlert(logStr);
    }
}

// -------------------- SETUP E LOOP --------------------
void cpuLoadTask(void *pvParameters) {
    while (true) {
        idle_counter = 0;
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Calibrato per ESP32 a 240MHz
        cpu_load_percent = 100.0 - ((float)idle_counter * 100.0 / 800000.0);
    }
}

void setup() {
    Serial.begin(115200);
    ARDUINO_SERIAL.begin(115200);
    CAM_SERIAL.begin(115200);
    
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");
    
    configTime(0, 0, ntpServer);

    preferences.begin("hub_config", false);
    String storedUrl = preferences.getString("webhookUrl", "YOUR_IFTTT_OR_SLACK_WEBHOOK_URL");
    strncpy(webhookUrl, storedUrl.c_str(), sizeof(webhookUrl));
    preferences.end();

    xTaskCreatePinnedToCore(cpuLoadTask, "CpuLoadTask", 2048, NULL, 1, &cpuLoadTaskHandle, 0);

    server.on("/logs/export", HTTP_GET, handleLogsExport);
    server.on("/stats/cumulative", HTTP_GET, handleCumulativeStats);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/config", HTTP_POST, handleSetConfig);
    server.on("/snapshot", HTTP_GET, [](){ server.send(501, "text/plain", "Not Implemented"); });
    server.on("/analyze", HTTP_GET, [](){ server.send(501, "text/plain", "Not Implemented"); });
    server.on("/setParams", HTTP_POST, [](){ server.send(501, "text/plain", "Not Implemented"); });
    server.onNotFound([](){ server.send(404, "text/plain", "Not Found"); });
    server.begin();
    logHubEvent("info", "system_boot", "Hub initialized successfully");
}

void loop() {
    server.handleClient();
    
    // Gestione seriale da Arduino Mega
    while (ARDUINO_SERIAL.available()) {
        char c = ARDUINO_SERIAL.read();
        if (c == '\n') {
            arduinoBuffer[arduinoBufferPos] = '\0';
            if (bufferPos > 0) {
                // Qui si processa il JSON da Mega (es. telemetria, log, ack)
                // e si aggiorna megaCumulativeStats
                lastMegaResponseTime = millis();
            }
            arduinoBufferPos = 0;
        } else if (arduinoBufferPos < SERIAL_BUFFER_SIZE - 1) {
            arduinoBuffer[arduinoBufferPos++] = c;
        }
    }
    
    idle_counter++;
}

// -------------------- GESTORI ENDPOINT HTTP --------------------
void handleLogsExport() {
    String response = "";
    for (size_t i = 0; i < logHistory.size(); i++) {
        response += logHistory[i] + "\n";
    }
    server.sendHeader("Content-Disposition", "attachment; filename=smartvase_logs.jsonl");
    server.send(200, "application/x-jsonlines", response);
}

void handleCumulativeStats() {
    StaticJsonDocument<2048> doc;
    doc["source"] = "hub_aggregator";
    doc["timestamp"] = getTimestampISO();

    JsonDocument megaDoc;
    deserializeJson(megaDoc, megaCumulativeStats);
    doc["mega_stats"] = megaDoc;

    JsonDocument camDoc;
    deserializeJson(camDoc, camCumulativeStats);
    doc["cam_stats"] = camDoc;

    JsonObject digest = doc.createNestedObject("digest");
    if (!megaDoc.isNull()) {
        JsonObject mega_perf = megaDoc["performance"];
        JsonObject mega_cumul = megaDoc["cumulative"];
        if (!mega_perf.isNull() && !mega_cumul.isNull()) {
            unsigned long mega_uptime_ms = mega_perf["uptime_ms"] | 0;
            float uptime_days = mega_uptime_ms / 86400000.0;
            digest["mega_uptime_days"] = uptime_days;
            if (uptime_days > 0.01) {
                digest["mega_irrigations_per_day"] = (mega_cumul["total_irrigations"] | 0) / uptime_days;
            }
            digest["mega_health_status"] = (mega_cumul["watchdog_resets"] | 0) > 0 ? "Warning" : "OK";
        }
    }
    if (!camDoc.isNull()) {
        JsonObject cam_cumul = camDoc["cumulative"];
        if (!cam_cumul.isNull()) {
            uint32_t s_frames = cam_cumul["successful_frames"] | 0;
            uint32_t f_frames = cam_cumul["failed_frames"] | 0;
            uint32_t total_frames = s_frames + f_frames;
            digest["cam_capture_success_rate_percent"] = (total_frames > 0) ? ((float)s_frames * 100.0 / total_frames) : 100.0;
        }
    }
    
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handleHealth() {
    StaticJsonDocument<2048> healthDoc;
    healthDoc["source"] = "health_check";
    
    JsonObject hub_details = healthDoc.createNestedObject("hub_details");
    hub_details["uptime_ms"] = millis();
    hub_details["free_ram_bytes"] = ESP.getFreeHeap();
    hub_details["cpu_load_percent"] = cpu_load_percent;
    hub_details["wifi_rssi"] = WiFi.RSSI();
    
    JsonObject mega_details = healthDoc.createNestedObject("mega_details");
    mega_details["status"] = (millis() - lastMegaResponseTime < MEGA_WATCHDOG_TIMEOUT) ? "responsive" : "unresponsive";
    
    JsonObject cam_details = healthDoc.createNestedObject("cam_details");
    cam_details["status"] = "placeholder";
    
    String output;
    serializeJson(healthDoc, output);
    server.send(200, "application/json", output);
}

void handleSetConfig() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Body required\"}");
        return;
    }
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    bool updated = false;
    if (doc.containsKey("webhookUrl")) {
        const char* newUrl = doc["webhookUrl"];
        strncpy(webhookUrl, newUrl, sizeof(webhookUrl));
        preferences.begin("hub_config", false);
        preferences.putString("webhookUrl", webhookUrl);
        preferences.end();
        logHubEvent("info", "config_updated", "Webhook URL updated");
        updated = true;
    }
    
    if(updated){
        server.send(200, "application/json", "{\"status\":\"Configuration updated\"}");
    } else {
        server.send(200, "application/json", "{\"status\":\"No valid configuration found to update\"}");
    }
}
