#include "HomeAssistantIntegration.h"

// Static instance pointer for callback
HomeAssistantIntegration* HomeAssistantIntegration::_instance = nullptr;

HomeAssistantIntegration::HomeAssistantIntegration(IrrigationController* controller)
    : _controller(controller),
      _wifiClient(nullptr),
      _mqttClient(nullptr),
      _port(MQTT_PORT),
      _lastReconnectAttempt(0),
      _lastStatusUpdate(0) {

    _instance = this;
}

HomeAssistantIntegration::~HomeAssistantIntegration() {
    if (_mqttClient) {
        _mqttClient->disconnect();
        delete _mqttClient;
    }
    if (_wifiClient) {
        delete _wifiClient;
    }
}

bool HomeAssistantIntegration::begin(const char* broker, uint16_t port,
                                     const char* user, const char* password) {
    DEBUG_PRINTLN("HomeAssistant: Initializing...");

    _broker = String(broker);
    _port = port;
    _user = user ? String(user) : "";
    _password = password ? String(password) : "";

    // Create WiFi client and MQTT client
    _wifiClient = new WiFiClient();
    _mqttClient = new PubSubClient(*_wifiClient);

    // Configure MQTT
    _mqttClient->setServer(_broker.c_str(), _port);
    _mqttClient->setCallback(mqttCallback);
    _mqttClient->setBufferSize(1024); // Increase buffer for discovery messages

    // Initial connection attempt
    connectMQTT();

    DEBUG_PRINTLN("HomeAssistant: Initialized");
    return true;
}

void HomeAssistantIntegration::connectMQTT() {
    if (!WiFi.isConnected()) {
        return;
    }

    if (_mqttClient->connected()) {
        return;
    }

    DEBUG_PRINT("HomeAssistant: Connecting to MQTT broker ");
    DEBUG_PRINTLN(_broker);

    bool connected = false;

    if (_user.length() > 0) {
        connected = _mqttClient->connect(MQTT_CLIENT_ID,
                                        _user.c_str(),
                                        _password.c_str());
    } else {
        connected = _mqttClient->connect(MQTT_CLIENT_ID);
    }

    if (connected) {
        DEBUG_PRINTLN("HomeAssistant: MQTT connected");

        // Subscribe to command topics
        subscribe();

        // Publish discovery messages
        publishDiscovery();

        // Publish initial state
        publishState();
        publishStatus();
    } else {
        DEBUG_PRINTF("HomeAssistant: MQTT connection failed, rc=%d\n",
                     _mqttClient->state());
    }
}

void HomeAssistantIntegration::subscribe() {
    // Subscribe to command topics
    String commandTopic = buildTopic("command");
    _mqttClient->subscribe(commandTopic.c_str());
    DEBUG_PRINTF("HomeAssistant: Subscribed to %s\n", commandTopic.c_str());

    String durationTopic = buildTopic("duration/set");
    _mqttClient->subscribe(durationTopic.c_str());
    DEBUG_PRINTF("HomeAssistant: Subscribed to %s\n", durationTopic.c_str());
}

void HomeAssistantIntegration::update() {
    if (!WiFi.isConnected()) {
        return;
    }

    unsigned long currentMillis = millis();

    // Handle MQTT reconnection
    if (!_mqttClient->connected()) {
        if (currentMillis - _lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL) {
            _lastReconnectAttempt = currentMillis;
            connectMQTT();
        }
    } else {
        // Process MQTT messages
        _mqttClient->loop();

        // Publish status periodically
        if (currentMillis - _lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
            _lastStatusUpdate = currentMillis;
            publishState();
            publishStatus();
        }
    }
}

void HomeAssistantIntegration::publishState() {
    if (!isConnected()) {
        return;
    }

    SystemStatus status = _controller->getStatus();
    String topic = buildTopic("state");
    String payload = status.irrigating ? "ON" : "OFF";

    _mqttClient->publish(topic.c_str(), payload.c_str(), true);
}

void HomeAssistantIntegration::publishStatus() {
    if (!isConnected()) {
        return;
    }

    SystemStatus status = _controller->getStatus();
    StaticJsonDocument<512> doc;

    doc["irrigating"] = status.irrigating;
    doc["manual_mode"] = status.manualMode;
    doc["wifi_connected"] = status.wifiConnected;
    doc["mqtt_connected"] = status.mqttConnected;

    if (status.irrigating) {
        doc["time_remaining"] = _controller->getTimeRemaining();
        doc["current_duration"] = status.currentDuration;
    }

    if (status.lastIrrigationTime > 0) {
        doc["last_irrigation"] = status.lastIrrigationTime;
    }

    unsigned long nextTime = _controller->getNextScheduledTime();
    if (nextTime > 0) {
        doc["next_scheduled"] = nextTime;
    }

    if (!status.lastError.isEmpty()) {
        doc["last_error"] = status.lastError;
    }

    String jsonString;
    serializeJson(doc, jsonString);

    String topic = buildTopic("status");
    _mqttClient->publish(topic.c_str(), jsonString.c_str(), true);
}

void HomeAssistantIntegration::publishSchedule() {
    if (!isConnected()) {
        return;
    }

    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count;
    _controller->getSchedules(schedules, count);

    DynamicJsonDocument doc(1024);
    JsonArray array = doc.createNestedArray("schedules");

    for (int i = 0; i < count; i++) {
        if (schedules[i].enabled) {
            JsonObject schedule = array.createNestedObject();
            schedule["index"] = i;
            schedule["hour"] = schedules[i].hour;
            schedule["minute"] = schedules[i].minute;
            schedule["duration"] = schedules[i].durationMinutes;
            schedule["weekdays"] = schedules[i].weekdays;
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);

    String topic = buildTopic("schedules");
    _mqttClient->publish(topic.c_str(), jsonString.c_str(), true);
}

void HomeAssistantIntegration::publishDiscovery() {
    if (!isConnected()) {
        return;
    }

    DEBUG_PRINTLN("HomeAssistant: Publishing discovery messages");

    // Main switch entity
    {
        StaticJsonDocument<512> doc;

        doc["name"] = HA_DEVICE_NAME;
        doc["unique_id"] = String(HA_DEVICE_ID) + "_switch";
        doc["state_topic"] = buildTopic("state");
        doc["command_topic"] = buildTopic("command");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["optimistic"] = false;
        doc["qos"] = 1;
        doc["retain"] = true;

        // Device information
        JsonObject device = doc.createNestedObject("device");
        JsonArray identifiers = device.createNestedArray("identifiers");
        identifiers.add(HA_DEVICE_ID);
        device["name"] = HA_DEVICE_NAME;
        device["model"] = "ESP32 Irrigation Controller";
        device["manufacturer"] = "DIY";
        device["sw_version"] = VERSION;

        String jsonString;
        serializeJson(doc, jsonString);

        String discoveryTopic = String(HA_DISCOVERY_PREFIX) + "/switch/" +
                               HA_DEVICE_ID + "/config";

        _mqttClient->publish(discoveryTopic.c_str(), jsonString.c_str(), true);
        DEBUG_PRINTLN("HomeAssistant: Switch discovery published");
    }

    // Status sensor
    {
        StaticJsonDocument<512> doc;

        doc["name"] = String(HA_DEVICE_NAME) + " Status";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_status";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ value_json.irrigating }}";
        doc["json_attributes_topic"] = buildTopic("status");
        doc["qos"] = 1;

        JsonObject device = doc.createNestedObject("device");
        JsonArray identifiers = device.createNestedArray("identifiers");
        identifiers.add(HA_DEVICE_ID);

        String jsonString;
        serializeJson(doc, jsonString);

        String discoveryTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" +
                               HA_DEVICE_ID + "_status/config";

        _mqttClient->publish(discoveryTopic.c_str(), jsonString.c_str(), true);
        DEBUG_PRINTLN("HomeAssistant: Status sensor discovery published");
    }

    // Duration number entity
    {
        StaticJsonDocument<512> doc;

        doc["name"] = String(HA_DEVICE_NAME) + " Duration";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_duration";
        doc["command_topic"] = buildTopic("duration/set");
        doc["state_topic"] = buildTopic("duration");
        doc["min"] = MIN_DURATION_MINUTES;
        doc["max"] = MAX_DURATION_MINUTES;
        doc["step"] = 5;
        doc["mode"] = "slider";
        doc["unit_of_measurement"] = "min";
        doc["qos"] = 1;

        JsonObject device = doc.createNestedObject("device");
        JsonArray identifiers = device.createNestedArray("identifiers");
        identifiers.add(HA_DEVICE_ID);

        String jsonString;
        serializeJson(doc, jsonString);

        String discoveryTopic = String(HA_DISCOVERY_PREFIX) + "/number/" +
                               HA_DEVICE_ID + "_duration/config";

        _mqttClient->publish(discoveryTopic.c_str(), jsonString.c_str(), true);
        DEBUG_PRINTLN("HomeAssistant: Duration number discovery published");
    }
}

void HomeAssistantIntegration::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance) {
        _instance->handleMQTTMessage(topic, payload, length);
    }
}

void HomeAssistantIntegration::handleMQTTMessage(char* topic, byte* payload, unsigned int length) {
    // Convert payload to string
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    DEBUG_PRINTF("HomeAssistant: Message received [%s]: %s\n", topic, message.c_str());

    String topicStr = String(topic);

    // Handle command topic
    if (topicStr.endsWith("/command")) {
        if (message == "ON") {
            DEBUG_PRINTLN("HomeAssistant: Starting irrigation via MQTT");
            _controller->startIrrigation(DEFAULT_DURATION_MINUTES);
        } else if (message == "OFF") {
            DEBUG_PRINTLN("HomeAssistant: Stopping irrigation via MQTT");
            _controller->stopIrrigation();
        }
        publishState();
    }
    // Handle duration set
    else if (topicStr.endsWith("/duration/set")) {
        int duration = message.toInt();
        if (duration >= MIN_DURATION_MINUTES && duration <= MAX_DURATION_MINUTES) {
            DEBUG_PRINTF("HomeAssistant: Setting duration to %d minutes\n", duration);
            // Duration will be used for next irrigation cycle
            // You might want to store this in a class variable or preferences
        }
    }
}

String HomeAssistantIntegration::buildTopic(const char* suffix) {
    return String(MQTT_BASE_TOPIC) + "/" + suffix;
}
