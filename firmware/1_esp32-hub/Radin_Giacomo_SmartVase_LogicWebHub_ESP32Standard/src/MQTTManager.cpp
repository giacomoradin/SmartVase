#include "MqttManager.h"
#include "esp_log.h"
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_random.h"
#include "esp_sntp.h"
#include "hivemq_ca_cert.h"

static const char *TAG = "MqttManager";

// Sotto questo epoch l'ora di sistema NON e' valida (orologio non sincronizzato
// via NTP): la verifica di validita' del certificato TLS fallirebbe.
#define NTP_VALID_EPOCH 1700000000UL

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

    // Genera un Client ID univoco basato sul MAC Address.
    // esp_read_mac legge dalla eFuse e funziona anche a radio spenta.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    _mqttClientId = "SmartVase_HUB_";
    char macSuffix[13]; // 6 byte * 2 char/byte + 1 null terminator
    snprintf(macSuffix, sizeof(macSuffix), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _mqttClientId += macSuffix;
    ESP_LOGI(TAG, "MQTT Client ID: %s", _mqttClientId.c_str());

    // Device ID STATICO da fonte unica HUB_DEVICE_ID (ConfigManager.h), coerente
    // con MainLogic. Il MAC resta usato solo per il Client ID MQTT qui sopra.
    _topicCommand = String("smartvase/") + HUB_DEVICE_ID + "/command/#"; // Sottoscrive a tutti i sottotopic
    _topicStatus  = String("smartvase/") + HUB_DEVICE_ID + "/status";
    ESP_LOGI(TAG, "Command Topic: %s", _topicCommand.c_str());
    ESP_LOGI(TAG, "Status Topic (LWT): %s", _topicStatus.c_str());


    // Transport in base alla porta: TLS (8883/8884, con CA cert + NTP) oppure
    // in chiaro (1883, broker locale: nessuna TLS, nessun NTP richiesto).
    const char* broker = _configManager.getMqttBroker();
    uint16_t port = _configManager.getMqttPort();
    if (port == 8883 || port == 8884) {
        _wifiClientSecure.setCACert(hivemq_ca_cert);
        _mqttClient.setClient(_wifiClientSecure);
        ESP_LOGI(TAG, "MQTT transport: TLS (porta %u)", port);
    } else {
        _mqttClient.setClient(_wifiClient);
        ESP_LOGW(TAG, "MQTT transport: IN CHIARO (porta %u) - nessuna TLS/NTP", port);
    }

    if (broker && strlen(broker) > 0 && port > 0) {
        _mqttClient.setServer(broker, port);
        ESP_LOGI(TAG, "MQTT Broker set to: %s:%d", broker, port);
    } else {
        ESP_LOGE(TAG, "MQTT Broker configuration missing or invalid!");
    }

    // Imposta la funzione di callback per i messaggi in arrivo
    _mqttClient.setCallback(MqttManager::mqttCallback);

    // Imposta la dimensione del buffer interno di PubSubClient (se necessario)
    _mqttClient.setBufferSize(MQTT_BUFFER_SIZE);

    // Bound al wait del CONNACK/letture: evita che un broker morto tenga il
    // task appeso a lungo (resilienza; default PubSubClient = 15 s).
    _mqttClient.setSocketTimeout(10);
}

// Funzione statica entry point per il Task FreeRTOS
void MqttManager::taskEntry(void* pvParameters) {
    MqttManager* instance = static_cast<MqttManager*>(pvParameters);
    // init() e' gia' chiamato dal setup() prima della creazione del task:
    // non lo si ripete per evitare doppia inizializzazione.
    instance->taskRun(); // Entra nel loop principale del task
}

// Funzione principale del Task (loop infinito)
void MqttManager::taskRun() {
    ESP_LOGI(TAG, "MqttManager Task Started.");
    MqttMessage msgToPublish;
    bool warnedNotConfigured = false;

    while (true) {
        // 0. Broker non configurato: nessun tentativo di connessione.
        //    Si svuota comunque la coda TX per non far accumulare messaggi.
        if (!isConfigured()) {
            if (!warnedNotConfigured) {
                warnedNotConfigured = true;
                ESP_LOGW(TAG, "Broker MQTT non configurato: pubblicazione disattivata.");
                Serial.println(F("[MQTT] Non configurato. Dalla CLI: set mqtt_broker <host>, set mqtt_user/mqtt_pass, save, reboot"));
            }
            while (xQueueReceive(_txQueue, &msgToPublish, 0) == pdPASS) { /* discard */ }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 1. Assicurati che il client MQTT sia connesso
        if (!_mqttClient.connected()) {
            // Tenta la riconnessione (con logica di backoff interno).
            // Nel frattempo la coda TX viene svuotata: la telemetria e'
            // periodica, accumularla offline non ha valore.
            reconnect();
            // Offline: scarta SOLO la telemetria (perde valore se vecchia) ma
            // mantieni in coda alarm/ack/log per inviarli alla riconnessione.
            // Bounded: un solo giro sui messaggi presenti ora.
            UBaseType_t pending = uxQueueMessagesWaiting(_txQueue);
            for (UBaseType_t i = 0; i < pending; ++i) {
                if (xQueueReceive(_txQueue, &msgToPublish, 0) != pdPASS) break;
                if (strstr(msgToPublish.topic, "/telemetry") != nullptr) continue; // scarta
                if (xQueueSend(_txQueue, &msgToPublish, 0) != pdPASS) break;        // rimetti in coda
            }
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

// Tenta la riconnessione al broker MQTT. NON bloccante: ritorna subito se il
// backoff non e' ancora scaduto (la cadenza la da' il vTaskDelay del taskRun),
// cosi' il task resta reattivo invece di dormire 5 s a tentativo.
bool MqttManager::reconnect() {
    // Niente WiFi: nessun tentativo, nessun delay bloccante.
    if (!WiFi.isConnected()) return false;

    // NTP serve SOLO per la TLS (validazione data del cert): con broker in
    // chiaro (1883) si salta del tutto, cosi' una rete locale offline puo'
    // connettersi lo stesso. Con l'ora a 1970 il cert HiveMQ risulta "not yet
    // valid": SNTP di default ritenta ~ogni ora, quindi se resta invalida forzo
    // un nuovo tentativo ogni 30 s riavviando SNTP.
    uint16_t mqttPort = _configManager.getMqttPort();
    bool needsTLS = (mqttPort == 8883 || mqttPort == 8884);
    if (needsTLS && time(nullptr) < NTP_VALID_EPOCH) {
        static uint32_t lastNtpKickMs = 0;
        uint32_t nowMs = millis();
        if (lastNtpKickMs == 0) {
            configTime(0, 0, "pool.ntp.org", "time.google.com"); // avvia SNTP
            lastNtpKickMs = nowMs;
            ESP_LOGW(TAG, "Ora non valida: NTP avviato, rimando il connect MQTT.");
        } else if (nowMs - lastNtpKickMs > 30000UL) {
            sntp_stop();
            configTime(0, 0, "pool.ntp.org", "time.google.com"); // forza nuovo tentativo
            lastNtpKickMs = nowMs;
            ESP_LOGW(TAG, "NTP ancora non sincronizzato: nuovo tentativo.");
        }
        return false;
    }

    // Backoff con tetto 30 s + jitter, gestito a timer (no delay bloccante).
    static uint32_t lastAttemptMs = 0;
    static uint32_t backoffMs     = 0;
    uint32_t now = millis();
    if (lastAttemptMs != 0 && (now - lastAttemptMs) < backoffMs) return false;
    lastAttemptMs = now;

    ESP_LOGI(TAG, "Attempting MQTT connection...");
    const char* user     = _configManager.getMqttUser();
    const char* password = _configManager.getMqttPassword();

    // LWT: alla disconnessione anomala il broker pubblica "offline" su /status.
    if (_mqttClient.connect(_mqttClientId.c_str(), user, password,
                            _topicStatus.c_str(), 1, true, "offline")) {
        ESP_LOGI(TAG, "MQTT Connected!");
        backoffMs = 0; // reset backoff a connessione riuscita

        String onlinePayload = "online";
        _mqttClient.publish(_topicStatus.c_str(), (uint8_t*)onlinePayload.c_str(),
                            onlinePayload.length(), true); // retain

        if (_mqttClient.subscribe(_topicCommand.c_str())) {
            ESP_LOGI(TAG, "Subscribed to command topic: %s", _topicCommand.c_str());
        } else {
            ESP_LOGE(TAG, "Failed to subscribe to command topic!");
        }
        return true;
    }

    // Backoff esponenziale (5→10→20→30 s) + jitter per evitare polling sincrono.
    backoffMs = (backoffMs == 0) ? 5000 : (backoffMs < 30000 ? backoffMs * 2 : 30000);
    backoffMs += (esp_random() % 1000);
    ESP_LOGW(TAG, "MQTT connect failed rc=%d, retry tra ~%lu ms",
             _mqttClient.state(), (unsigned long)backoffMs);
    return false;
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
