#include "MqttManager.h"
#include "esp_log.h"
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "hivemq_ca_cert.h"

static const char *TAG = "MqttManager";

// Cert HiveMQ Cloud (ISRG Root X1). Definito in include/hivemq_ca_cert.h
// (shared con la CAM via infra/hivemq_ca_cert.h).
static const char* hivemq_ca_cert = SMARTVASE_HIVEMQ_CA_CERT;

// Inizializza il puntatore statico all'istanza
MqttManager* MqttManager::_instance = nullptr;

// --- Implementazione Metodi Classe MqttManager ---

MqttManager::MqttManager(QueueHandle_t txQueue, QueueHandle_t rxQueue, ConfigManager& configManager)
    : _txQueue(txQueue),
      _rxQueue(rxQueue),
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

    // Controlla se l'istanza e la coda sono valide
    if (!_instance || !_instance->_rxQueue) {
        ESP_LOGE(TAG, "Callback invoked without a valid instance or RX queue!");
        return;
    }

    // Controlla che il payload non sia troppo grande per la nostra struct
    if (length >= sizeof(MqttCommand::payload)) {
        ESP_LOGE(TAG, "Incoming MQTT message payload too large (%d bytes)! Max: %d. Discarding.", length, sizeof(MqttCommand::payload) - 1);
        return;
    }

    // Crea il comando da inviare alla coda
    MqttCommand cmd;
    cmd.timestamp = millis(); // Usa millis() o un'altra fonte di tempo

    // Copia il topic e il payload nel comando
    strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
    cmd.topic[sizeof(cmd.topic) - 1] = '\0'; // Assicura terminazione nulla

    memcpy(cmd.payload, payload, length);
    cmd.payload[length] = '\0'; // Assicura terminazione nulla

    ESP_LOGD(TAG, "Payload: %s", cmd.payload);

    // Invia il comando alla coda di MainLogic per il processamento
    // Usa un timeout breve per non bloccare il callback MQTT a lungo
    if (xQueueSend(_instance->_rxQueue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to queue incoming MQTT command. RX Queue may be full.");
    }
}
