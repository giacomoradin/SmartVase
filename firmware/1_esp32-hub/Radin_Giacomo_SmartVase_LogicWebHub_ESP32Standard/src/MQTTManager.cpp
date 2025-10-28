#include "MqttManager.h"
#include "esp_log.h"
#include <esp_wifi.h> // Per MAC address

// Tag per i log di questo modulo
static const char *TAG = "MqttManager";

// Certificato CA di HiveMQ Cloud (fornito da Fia)
// IMPORTANTE: Assicurarsi che questo certificato sia valido e corretto!
const char* hivemq_ca_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"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\n" \
"-----END CERTIFICATE-----\n";

// Inizializza il puntatore statico all'istanza
MqttManager* MqttManager::_instance = nullptr;

// --- Implementazione Metodi Classe MqttManager ---

MqttManager::MqttManager(QueueHandle_t txQueue, ConfigManager& configManager)
    : _txQueue(txQueue),
      _configManager(configManager),
      _mqttClient(_wifiClientSecure) // Passa il client sicuro al costruttore di PubSubClient
{
    _instance = this; // Salva il puntatore all'istanza per il callback statico
}

void MqttManager::init() {
    ESP_LOGI(TAG, "Initializing MQTT Manager...");

    // Genera un Client ID univoco basato sul MAC Address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    _mqttClientId = "SmartVase_HUB_";
    char macSuffix[13]; // 6 byte * 2 char/byte + 1 null terminator
    snprintf(macSuffix, sizeof(macSuffix), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _mqttClientId += macSuffix;
    ESP_LOGI(TAG, "MQTT Client ID: %s", _mqttClientId.c_str());

    // Costruisci i topic specifici per questo device
    // Assumiamo che il device ID per MQTT sia HUB_ + ultime 3 coppie del MAC
    char deviceIdSuffix[7];
    snprintf(deviceIdSuffix, sizeof(deviceIdSuffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    _topicCommand = "smartvase/HUB_" + String(deviceIdSuffix) + "/command/#"; // Sottoscrive a tutti i sottotopic
    _topicStatus = "smartvase/HUB_" + String(deviceIdSuffix) + "/status";
    ESP_LOGI(TAG, "Command Topic: %s", _topicCommand.c_str());
    ESP_LOGI(TAG, "Status Topic (LWT): %s", _topicStatus.c_str());


    // Configura il client WiFi sicuro con il certificato CA
    _wifiClientSecure.setCACert(hivemq_ca_cert);
    // Potrebbe essere necessario configurare anche l'ora (NTP) per la validazione del certificato
    // configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Imposta il server MQTT (broker e porta) dal ConfigManager
    const char* broker = _configManager.getMqttBroker();
    uint16_t port = _configManager.getMqttPort();
    if (broker && strlen(broker) > 0 && port > 0) {
        _mqttClient.setServer(broker, port);
        ESP_LOGI(TAG, "MQTT Broker set to: %s:%d", broker, port);
    } else {
        ESP_LOGE(TAG, "MQTT Broker configuration missing or invalid!");
        // Gestire questo errore? Riprovare a caricare config? Per ora procediamo...
    }

    // Imposta la funzione di callback per i messaggi in arrivo
    _mqttClient.setCallback(MqttManager::mqttCallback);

    // Imposta la dimensione del buffer interno di PubSubClient (se necessario)
    _mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
}

// Funzione statica entry point per il Task FreeRTOS
void MqttManager::taskEntry(void* pvParameters) {
    MqttManager* instance = static_cast<MqttManager*>(pvParameters);
    instance->init(); // Chiama l'inizializzazione specifica
    instance->taskRun(); // Entra nel loop principale del task
}

// Funzione principale del Task (loop infinito)
void MqttManager::taskRun() {
    ESP_LOGI(TAG, "MqttManager Task Started.");
    MqttMessage msgToPublish;

    while (true) {
        // 1. Assicurati che il client MQTT sia connesso
        if (!_mqttClient.connected()) {
            // Tenta la riconnessione (con logica di backoff interno)
            reconnect();
        } else {
            // 2. Se connesso, processa i messaggi MQTT in arrivo e mantieni la connessione attiva
            _mqttClient.loop();

            // 3. Controlla se ci sono messaggi JSON da pubblicare dalla coda _txQueue
            //    Controlla senza bloccare (timeout 0)
            if (xQueueReceive(_txQueue, &msgToPublish, 0) == pdPASS) {
                ESP_LOGD(TAG, "Publishing message from TX queue to topic: %s", msgToPublish.topic);
                //ESP_LOGV(TAG, "Payload: %s", msgToPublish.payload); // Logga payload solo a livello Verbose

                // Pubblica il messaggio
                // Parametri: topic, payload (byte array), lunghezza payload, retain (false)
                if (!_mqttClient.publish(msgToPublish.topic, (uint8_t*)msgToPublish.payload, strlen(msgToPublish.payload), false)) {
                    ESP_LOGE(TAG, "MQTT Publish failed! Topic: %s", msgToPublish.topic);
                    // Qui potresti rimettere il messaggio in coda o scartarlo
                } else {
                     ESP_LOGD(TAG, "Publish successful.");
                }
            }
        } // Fine else (connesso)

        // 4. Pausa del task per cedere CPU
        //    La frequenza dipende da quanto reattivo deve essere il client MQTT
        //    e da quanto spesso ci aspettiamo messaggi da pubblicare.
        vTaskDelay(pdMS_TO_TICKS(100)); // Controlla/Processa ogni 100ms
    }
}

// Tenta la riconnessione al broker MQTT (logica presa da Fia)
bool MqttManager::reconnect() {
    // Non tentare se il WiFi non è connesso
    if (!WiFi.isConnected()) {
        // ESP_LOGV(TAG, "WiFi not connected, skipping MQTT reconnect attempt."); // Log troppo frequente?
        // Aggiungi un delay più lungo qui per non sprecare CPU se il WiFi è giù
        vTaskDelay(pdMS_TO_TICKS(5000)); // Attendi 5 secondi prima di riprovare
        return false;
    }

    ESP_LOGI(TAG, "Attempting MQTT connection...");
    const char* user = _configManager.getMqttUser();
    const char* password = _configManager.getMqttPassword();

    // Costruisci il messaggio Last Will and Testament (LWT)
    // Se l'Hub si disconnette in modo anomalo, il broker pubblicherà "offline" sul topic di stato
    String lwtPayload = "offline";

    // Tenta la connessione con ClientID, utente, password e LWT
    if (_mqttClient.connect(_mqttClientId.c_str(), user, password, _topicStatus.c_str(), 1, true, lwtPayload.c_str())) {
        ESP_LOGI(TAG, "MQTT Connected!");

        // Pubblica lo stato "online" sul topic di stato (usando retain=true)
        // Così chi si connette dopo saprà subito che siamo online
        String onlinePayload = "online";
        _mqttClient.publish(_topicStatus.c_str(), (uint8_t*)onlinePayload.c_str(), onlinePayload.length(), true); // retain = true

        // Sottoscrivi al topic dei comandi
        if (_mqttClient.subscribe(_topicCommand.c_str())) {
            ESP_LOGI(TAG, "Subscribed to command topic: %s", _topicCommand.c_str());
        } else {
            ESP_LOGE(TAG, "Failed to subscribe to command topic!");
            // Disconnetti? O procedi comunque?
        }
        return true; // Connessione riuscita
    } else {
        // Stampa l'errore specifico di PubSubClient
        ESP_LOGW(TAG, "MQTT Connection Failed, rc=%d. Retrying in 5 seconds...", _mqttClient.state());
        // Attendi 5 secondi prima del prossimo tentativo
        vTaskDelay(pdMS_TO_TICKS(5000));
        return false; // Connessione fallita
    }
}

// Callback per i messaggi MQTT in arrivo (DEVE essere statica)
void MqttManager::mqttCallback(char* topic, byte* payload, unsigned int length) {
    ESP_LOGI(TAG, "Message arrived [%s]", topic);

    // Copia il payload in un buffer locale perché il puntatore originale
    // potrebbe non essere più valido dopo questa funzione.
    // Assicurati che il buffer sia abbastanza grande!
    char messageBuffer[MQTT_BUFFER_SIZE];
    if (length >= sizeof(messageBuffer)) {
        ESP_LOGE(TAG, "Incoming MQTT message too large (%d bytes)! Max buffer: %d. Discarding.", length, sizeof(messageBuffer));
        return;
    }
    memcpy(messageBuffer, payload, length);
    messageBuffer[length] = '\0'; // Aggiungi terminatore nullo per trattarlo come stringa C

    ESP_LOGI(TAG, "Payload: %s", messageBuffer);

    // --- Qui va la logica per processare il comando ---
    // 1. Parsificare il JSON ricevuto (messageBuffer) usando ArduinoJson
    // 2. Validare il comando
    // 3. Creare un messaggio/struct appropriato per il Task MainLogic
    // 4. Inviare il messaggio alla coda _mqttRxQueue (che dobbiamo ancora creare)

    // Esempio Placeholder:
    // StaticJsonDocument<256> doc; // Dimensione da adattare
    // DeserializationError error = deserializeJson(doc, messageBuffer);
    // if (error) {
    //     ESP_LOGE(TAG, "deserializeJson() failed: %s", error.c_str());
    //     return;
    // }
    // const char* commandType = doc["type"];
    // if (commandType) {
    //     if (strcmp(commandType, "setPlantConfig") == 0) {
    //         // Estrai parametri, crea messaggio per MainLogic, invia a _mqttRxQueue
    //         ESP_LOGI(TAG,"Received setPlantConfig command");
    //     } else if (...) { ... }
    // } else {
    //     ESP_LOGW(TAG,"Received MQTT message without 'type' field.");
    // }

    // Per ora, logghiamo solo il messaggio ricevuto.
}
