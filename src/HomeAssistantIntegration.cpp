#include "HomeAssistantIntegration.h"
#include "NodeManager.h"

// Static instance pointer for callback
HomeAssistantIntegration* HomeAssistantIntegration::_instance = nullptr;

HomeAssistantIntegration::HomeAssistantIntegration(IrrigationController* controller)
    : _controller(controller),
      _nodeManager(nullptr),
      _wifiClient(nullptr),
      _mqttClient(nullptr),
      _port(MQTT_PORT),
      _lastReconnectAttempt(0),
      _lastStatusUpdate(0),
      _lastDiscoveredCount(0),
      _systemEnabled(true),
      _currentMode("auto"),
      _lastIrrigatingState(false),
      _lastFastStatusUpdate(0),
      _needsDiscoveryPublish(true),
      _lastDiscoveryVersion("") {

    _instance = this;

    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        _channelDuration[i] = DEFAULT_DURATION_MINUTES;
        _discoveredChannels[i] = false;
        _retryPending[i] = false;
        _commandSentTime[i] = 0;
    }
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

    // Try to load saved credentials first
    bool hasCredentials = false;
    if (broker != nullptr) {
        _broker = String(broker);
        _port = port;
        _user = user ? String(user) : "";
        _password = password ? String(password) : "";
        hasCredentials = true;
    } else {
        hasCredentials = loadCredentials();
    }

    if (!hasCredentials || _broker.length() == 0) {
        DEBUG_PRINTLN("HomeAssistant: No MQTT credentials configured");
        return false;
    }

    _wifiClient = new WiFiClient();
    _mqttClient = new PubSubClient(*_wifiClient);

    _mqttClient->setServer(_broker.c_str(), _port);
    _mqttClient->setCallback(mqttCallback);
    _mqttClient->setBufferSize(1024);

    connectMQTT();

    DEBUG_PRINTLN("HomeAssistant: Initialized");
    return true;
}

bool HomeAssistantIntegration::loadCredentials() {
    if (!SPIFFS.exists(MQTT_CREDENTIALS_FILE)) {
        DEBUG_PRINTLN("HomeAssistant: No credentials file found");
        return false;
    }

    File file = SPIFFS.open(MQTT_CREDENTIALS_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("HomeAssistant: Failed to open credentials file");
        return false;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("HomeAssistant: Failed to parse credentials: %s\n", error.c_str());
        return false;
    }

    _broker = doc["broker"] | "";
    _port = doc["port"] | 1883;
    _user = doc["user"] | "";
    _password = doc["password"] | "";

    if (_broker.length() > 0) {
        DEBUG_PRINTF("HomeAssistant: Loaded credentials for broker: %s\n", _broker.c_str());
        return true;
    }

    return false;
}

bool HomeAssistantIntegration::saveCredentials(const String& broker, uint16_t port,
                                                const String& user, const String& password) {
    StaticJsonDocument<512> doc;
    doc["broker"] = broker;
    doc["port"] = port;
    doc["user"] = user;
    doc["password"] = password;

    File file = SPIFFS.open(MQTT_CREDENTIALS_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("HomeAssistant: Failed to open credentials file for writing");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        DEBUG_PRINTLN("HomeAssistant: Failed to write credentials");
        file.close();
        return false;
    }

    file.close();
    DEBUG_PRINTLN("HomeAssistant: Credentials saved successfully");
    return true;
}

bool HomeAssistantIntegration::testConnection(const String& broker, uint16_t port,
                                               const String& user, const String& password) {
    DEBUG_PRINTF("HomeAssistant: Testing connection to %s:%d\n", broker.c_str(), port);

    WiFiClient testClient;
    PubSubClient testMqtt(testClient);
    testMqtt.setServer(broker.c_str(), port);

    bool connected = false;
    if (user.length() > 0) {
        connected = testMqtt.connect("irrigation_test", user.c_str(), password.c_str());
    } else {
        connected = testMqtt.connect("irrigation_test");
    }

    if (connected) {
        DEBUG_PRINTLN("HomeAssistant: Test connection successful");
        testMqtt.disconnect();
        return true;
    } else {
        DEBUG_PRINTF("HomeAssistant: Test connection failed, state: %d\n", testMqtt.state());
        return false;
    }
}

// ============================================================================
// MQTT Connection with LWT
// ============================================================================

void HomeAssistantIntegration::connectMQTT() {
    if (!WiFi.isConnected()) return;
    if (_mqttClient->connected()) return;

    DEBUG_PRINT("HomeAssistant: Connecting to MQTT broker ");
    DEBUG_PRINTLN(_broker);

    String availTopic = buildTopic("availability");
    bool connected = false;

    if (_user.length() > 0) {
        connected = _mqttClient->connect(MQTT_CLIENT_ID,
                                         _user.c_str(), _password.c_str(),
                                         availTopic.c_str(), 1, true, "offline");
    } else {
        connected = _mqttClient->connect(MQTT_CLIENT_ID,
                                         nullptr, nullptr,
                                         availTopic.c_str(), 1, true, "offline");
    }

    if (connected) {
        DEBUG_PRINTLN("HomeAssistant: MQTT connected");

        // Publish online availability
        _mqttClient->publish(availTopic.c_str(), "online", true);

        // Subscribe to command topics
        subscribe();

        // Publish discovery if needed
        if (_needsDiscoveryPublish || _lastDiscoveryVersion != VERSION) {
            publishDiscovery();
            _needsDiscoveryPublish = false;
            _lastDiscoveryVersion = VERSION;
        }

        // Publish initial state
        publishState();
        publishStatus();
        publishChannelStates();
        publishIndividualStatus();
        publishSchedule();
        publishModeState();
    } else {
        DEBUG_PRINTF("HomeAssistant: MQTT connection failed, rc=%d\n",
                     _mqttClient->state());
    }
}

void HomeAssistantIntegration::setNodeManager(NodeManager* nm) {
    _nodeManager = nm;
}

// ============================================================================
// Subscribe
// ============================================================================

void HomeAssistantIntegration::subscribe() {
    // System enable (master switch)
    String commandTopic = buildTopic("command");
    _mqttClient->subscribe(commandTopic.c_str());
    DEBUG_PRINTF("HomeAssistant: Subscribed to %s\n", commandTopic.c_str());

    // Global duration
    String durationTopic = buildTopic("duration/set");
    _mqttClient->subscribe(durationTopic.c_str());

    // Mode select
    String modeTopic = buildTopic("mode/set");
    _mqttClient->subscribe(modeTopic.c_str());

    // Schedule management
    String skipTopic = buildTopic("schedule/skip");
    _mqttClient->subscribe(skipTopic.c_str());
    String unskipTopic = buildTopic("schedule/unskip");
    _mqttClient->subscribe(unskipTopic.c_str());
    String setTopic = buildTopic("schedule/set");
    _mqttClient->subscribe(setTopic.c_str());
    String deleteTopic = buildTopic("schedule/delete");
    _mqttClient->subscribe(deleteTopic.c_str());

    // Per-channel topics
    for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
        if (_controller->isChannelEnabled(ch)) {
            String chCmd = buildTopic(("channel/" + String(ch) + "/command").c_str());
            _mqttClient->subscribe(chCmd.c_str());
            String chDur = buildTopic(("channel/" + String(ch) + "/duration/set").c_str());
            _mqttClient->subscribe(chDur.c_str());
        }
    }

    DEBUG_PRINTLN("HomeAssistant: Subscriptions complete");
}

// ============================================================================
// Update Loop
// ============================================================================

void HomeAssistantIntegration::update() {
    if (!_mqttClient) return;
    if (!WiFi.isConnected()) return;

    unsigned long currentMillis = millis();

    // Handle MQTT reconnection
    if (!_mqttClient->connected()) {
        if (currentMillis - _lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL) {
            _lastReconnectAttempt = currentMillis;
            connectMQTT();
        }
        return;
    }

    // Process MQTT messages
    _mqttClient->loop();

    // Change detection — immediate publish on irrigation state change
    bool currentIrrigating = _controller->isIrrigating();
    if (currentIrrigating != _lastIrrigatingState) {
        _lastIrrigatingState = currentIrrigating;
        publishChannelStates();
        publishIndividualStatus();
        publishState();
    }

    // Reconciliation — retry failed channel starts
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        if (_retryPending[i] && _commandSentTime[i] > 0) {
            if (currentMillis - _commandSentTime[i] > 10000) {
                // 10s elapsed since ON command, channel not running
                if (!_controller->isChannelIrrigating(i + 1)) {
                    DEBUG_PRINTF("HomeAssistant: Channel %d retry start (reconciliation)\n", i + 1);
                    _controller->startIrrigation(i + 1, _channelDuration[i]);
                    _retryPending[i] = false;

                    // Check again after brief delay (will be caught next loop)
                    _commandSentTime[i] = currentMillis;
                } else {
                    _retryPending[i] = false;
                    _commandSentTime[i] = 0;
                }
            }
        }
    }

    // Fast cycle during irrigation (30s)
    if (currentIrrigating) {
        if (currentMillis - _lastFastStatusUpdate >= 30000) {
            _lastFastStatusUpdate = currentMillis;
            publishChannelStates();
            publishIndividualStatus();
        }
    }

    // Standard 60s cycle
    if (currentMillis - _lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
        _lastStatusUpdate = currentMillis;
        publishState();
        publishStatus();
        publishChannelStates();
        publishIndividualStatus();

        // Publish per-channel availability for virtual channels
        if (_nodeManager) {
            for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
                if (!_controller->isChannelEnabled(ch)) continue;
                uint8_t idx = ch - 1;
                if (idx >= NUM_LOCAL_CHANNELS) {
                    // Virtual channel — derive availability from slave online status
                    const NodePeer* slave = nullptr;
                    for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                        const NodePeer* sl = _nodeManager->getSlave(s);
                        if (sl && ch >= sl->base_virtual_ch &&
                            ch < sl->base_virtual_ch + sl->num_channels) {
                            slave = sl;
                            break;
                        }
                    }
                    String availTopic = buildTopic(("channel/" + String(ch) + "/availability").c_str());
                    if (slave) {
                        _mqttClient->publish(availTopic.c_str(),
                                             slave->online ? "online" : "offline", true);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Discovery
// ============================================================================

void HomeAssistantIntegration::addDeviceBlock(JsonDocument& doc) {
    JsonObject device = doc.createNestedObject("device");
    JsonArray identifiers = device.createNestedArray("identifiers");
    identifiers.add(HA_DEVICE_ID);
    device["name"] = HA_DEVICE_NAME;
    device["model"] = "ESP32 Irrigation Controller";
    device["manufacturer"] = "OpsCraft";
    device["sw_version"] = VERSION;
}

void HomeAssistantIntegration::publishDiscovery() {
    if (!isConnected()) return;

    DEBUG_PRINTLN("HomeAssistant: Publishing discovery messages");

    String availTopic = buildTopic("availability");

    // === 1. System Enable switch (master arm/disarm) ===
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " System";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_switch";
        doc["state_topic"] = buildTopic("state");
        doc["command_topic"] = buildTopic("command");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["availability_topic"] = availTopic;
        doc["optimistic"] = false;
        doc["qos"] = 1;
        doc["retain"] = true;
        doc["icon"] = "mdi:water-pump";
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/switch/" + HA_DEVICE_ID + "/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // === 2. Mode select ===
    publishModeSelectDiscovery();
    delay(50);

    // === 3. Global sensors ===
    publishGlobalSensorDiscovery();
    delay(50);

    // === 4. Global duration number ===
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Duration";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_duration";
        doc["command_topic"] = buildTopic("duration/set");
        doc["state_topic"] = buildTopic("duration");
        doc["min"] = MIN_DURATION_MINUTES;
        doc["max"] = MAX_DURATION_MINUTES;
        doc["step"] = 1;
        doc["mode"] = "slider";
        doc["unit_of_measurement"] = "min";
        doc["availability_topic"] = availTopic;
        doc["qos"] = 1;
        doc["icon"] = "mdi:timer-outline";
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/number/" + HA_DEVICE_ID + "_duration/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // === 5. Per-channel entities ===
    for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
        if (_controller->isChannelEnabled(ch)) {
            publishChannelSwitchDiscovery(ch);
            delay(50);
            publishChannelDurationDiscovery(ch);
            delay(50);
            publishChannelRuntimeDiscovery(ch);
            delay(50);
            _discoveredChannels[ch - 1] = true;
        } else if (_discoveredChannels[ch - 1]) {
            // Channel was previously discovered but now disabled — remove
            String chId = String(HA_DEVICE_ID) + "_ch" + String(ch);
            String switchTopic = String(HA_DISCOVERY_PREFIX) + "/switch/" + chId + "/config";
            _mqttClient->publish(switchTopic.c_str(), "", true);  // Empty payload = remove
            String durTopic = String(HA_DISCOVERY_PREFIX) + "/number/" + chId + "_duration/config";
            _mqttClient->publish(durTopic.c_str(), "", true);
            String runTopic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + chId + "_running/config";
            _mqttClient->publish(runTopic.c_str(), "", true);
            String timeTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + chId + "_time_remaining/config";
            _mqttClient->publish(timeTopic.c_str(), "", true);
            _discoveredChannels[ch - 1] = false;
            delay(50);
        }
    }

    DEBUG_PRINTLN("HomeAssistant: Discovery complete");
}

void HomeAssistantIntegration::publishModeSelectDiscovery() {
    StaticJsonDocument<512> doc;
    String availTopic = buildTopic("availability");

    doc["name"] = String(HA_DEVICE_NAME) + " Mode";
    doc["unique_id"] = String(HA_DEVICE_ID) + "_mode";
    doc["command_topic"] = buildTopic("mode/set");
    doc["state_topic"] = buildTopic("mode");
    doc["availability_topic"] = availTopic;
    JsonArray options = doc.createNestedArray("options");
    options.add("auto");
    options.add("manual");
    options.add("disabled");
    doc["icon"] = "mdi:tune";
    addDeviceBlock(doc);

    String json;
    serializeJson(doc, json);
    String topic = String(HA_DISCOVERY_PREFIX) + "/select/" + HA_DEVICE_ID + "_mode/config";
    _mqttClient->publish(topic.c_str(), json.c_str(), true);
}

void HomeAssistantIntegration::publishGlobalSensorDiscovery() {
    String availTopic = buildTopic("availability");

    // Irrigating binary sensor
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Irrigating";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_irrigating";
        doc["state_topic"] = buildTopic("status/irrigating");
        doc["payload_on"] = "true";
        doc["payload_off"] = "false";
        doc["device_class"] = "running";
        doc["availability_topic"] = availTopic;
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + HA_DEVICE_ID + "_irrigating/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // Time remaining sensor
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Time Remaining";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_time_remaining";
        doc["state_topic"] = buildTopic("status/time_remaining");
        doc["device_class"] = "duration";
        doc["state_class"] = "measurement";
        doc["unit_of_measurement"] = "s";
        doc["icon"] = "mdi:timer-sand";
        doc["availability_topic"] = availTopic;
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + HA_DEVICE_ID + "_time_remaining/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // Next scheduled sensor (timestamp)
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Next Scheduled";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_next_scheduled";
        doc["state_topic"] = buildTopic("status/next_scheduled");
        doc["device_class"] = "timestamp";
        doc["availability_topic"] = availTopic;
        doc["icon"] = "mdi:calendar-clock";
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + HA_DEVICE_ID + "_next_scheduled/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // Status sensor (JSON blob — backward compat)
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Status";
        doc["unique_id"] = String(HA_DEVICE_ID) + "_status";
        doc["state_topic"] = buildTopic("status");
        doc["value_template"] = "{{ value_json.irrigating }}";
        doc["json_attributes_topic"] = buildTopic("status");
        doc["availability_topic"] = availTopic;
        doc["qos"] = 1;
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + HA_DEVICE_ID + "_status/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
}

void HomeAssistantIntegration::publishChannelSwitchDiscovery(uint8_t channel) {
    StaticJsonDocument<512> doc;
    String chId = String(HA_DEVICE_ID) + "_ch" + String(channel);
    String chBase = "channel/" + String(channel);
    String availTopic = buildTopic("availability");

    doc["name"] = String(HA_DEVICE_NAME) + " Channel " + String(channel);
    doc["unique_id"] = chId + "_switch";
    doc["state_topic"] = buildTopic((chBase + "/state").c_str());
    doc["command_topic"] = buildTopic((chBase + "/command").c_str());
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["optimistic"] = false;
    doc["qos"] = 1;
    doc["icon"] = "mdi:water";

    // Virtual channels get per-channel availability
    uint8_t idx = channel - 1;
    if (idx >= NUM_LOCAL_CHANNELS && _nodeManager) {
        doc["availability_topic"] = buildTopic((chBase + "/availability").c_str());
    } else {
        doc["availability_topic"] = availTopic;
    }

    addDeviceBlock(doc);

    String json;
    serializeJson(doc, json);
    String topic = String(HA_DISCOVERY_PREFIX) + "/switch/" + chId + "/config";
    _mqttClient->publish(topic.c_str(), json.c_str(), true);
}

void HomeAssistantIntegration::publishChannelDurationDiscovery(uint8_t channel) {
    StaticJsonDocument<512> doc;
    String chId = String(HA_DEVICE_ID) + "_ch" + String(channel);
    String chBase = "channel/" + String(channel);
    String availTopic = buildTopic("availability");

    doc["name"] = String(HA_DEVICE_NAME) + " Ch" + String(channel) + " Duration";
    doc["unique_id"] = chId + "_duration";
    doc["command_topic"] = buildTopic((chBase + "/duration/set").c_str());
    doc["state_topic"] = buildTopic((chBase + "/duration").c_str());
    doc["min"] = MIN_DURATION_MINUTES;
    doc["max"] = MAX_DURATION_MINUTES;
    doc["step"] = 1;
    doc["mode"] = "slider";
    doc["unit_of_measurement"] = "min";
    doc["availability_topic"] = availTopic;
    doc["icon"] = "mdi:timer-outline";
    addDeviceBlock(doc);

    String json;
    serializeJson(doc, json);
    String topic = String(HA_DISCOVERY_PREFIX) + "/number/" + chId + "_duration/config";
    _mqttClient->publish(topic.c_str(), json.c_str(), true);
}

void HomeAssistantIntegration::publishChannelRuntimeDiscovery(uint8_t channel) {
    String chId = String(HA_DEVICE_ID) + "_ch" + String(channel);
    String chBase = "channel/" + String(channel);
    String availTopic = buildTopic("availability");

    // Running binary sensor
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Ch" + String(channel) + " Running";
        doc["unique_id"] = chId + "_running";
        doc["state_topic"] = buildTopic((chBase + "/running").c_str());
        doc["payload_on"] = "true";
        doc["payload_off"] = "false";
        doc["device_class"] = "running";
        doc["availability_topic"] = availTopic;
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + chId + "_running/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
    delay(50);

    // Time remaining sensor
    {
        StaticJsonDocument<512> doc;
        doc["name"] = String(HA_DEVICE_NAME) + " Ch" + String(channel) + " Time Left";
        doc["unique_id"] = chId + "_time_remaining";
        doc["state_topic"] = buildTopic((chBase + "/time_remaining").c_str());
        doc["device_class"] = "duration";
        doc["state_class"] = "measurement";
        doc["unit_of_measurement"] = "s";
        doc["icon"] = "mdi:timer-sand";
        doc["availability_topic"] = availTopic;
        addDeviceBlock(doc);

        String json;
        serializeJson(doc, json);
        String topic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + chId + "_time_remaining/config";
        _mqttClient->publish(topic.c_str(), json.c_str(), true);
    }
}

void HomeAssistantIntegration::refreshDiscovery() {
    _needsDiscoveryPublish = true;
    if (isConnected()) {
        publishDiscovery();
        _needsDiscoveryPublish = false;
        // Re-subscribe to pick up new channels
        subscribe();
    }
}

void HomeAssistantIntegration::removeStaleDiscovery() {
    for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
        if (_discoveredChannels[ch - 1] && !_controller->isChannelEnabled(ch)) {
            String chId = String(HA_DEVICE_ID) + "_ch" + String(ch);
            // Publish empty retained payload to remove from HA
            String switchTopic = String(HA_DISCOVERY_PREFIX) + "/switch/" + chId + "/config";
            _mqttClient->publish(switchTopic.c_str(), "", true);
            String durTopic = String(HA_DISCOVERY_PREFIX) + "/number/" + chId + "_duration/config";
            _mqttClient->publish(durTopic.c_str(), "", true);
            String runTopic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + chId + "_running/config";
            _mqttClient->publish(runTopic.c_str(), "", true);
            String timeTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + chId + "_time_remaining/config";
            _mqttClient->publish(timeTopic.c_str(), "", true);
            _discoveredChannels[ch - 1] = false;
            delay(50);
        }
    }
}

// ============================================================================
// Message Handler
// ============================================================================

void HomeAssistantIntegration::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance) {
        _instance->handleMQTTMessage(topic, payload, length);
    }
}

void HomeAssistantIntegration::handleMQTTMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    DEBUG_PRINTF("HomeAssistant: Message received [%s]: %s\n", topic, message.c_str());

    String topicStr = String(topic);
    String baseTopic = String(MQTT_BASE_TOPIC) + "/";

    // --- Per-channel command: .../channel/{N}/command ---
    if (topicStr.indexOf("/channel/") >= 0 && topicStr.endsWith("/command")) {
        // Extract channel number
        int chStart = topicStr.indexOf("/channel/") + 9;
        int chEnd = topicStr.indexOf("/", chStart);
        if (chEnd > chStart) {
            uint8_t ch = topicStr.substring(chStart, chEnd).toInt();
            if (ch >= 1 && ch <= MAX_CHANNELS) {
                handleChannelCommand(ch, message);
                return;
            }
        }
    }

    // --- Per-channel duration: .../channel/{N}/duration/set ---
    if (topicStr.indexOf("/channel/") >= 0 && topicStr.endsWith("/duration/set")) {
        int chStart = topicStr.indexOf("/channel/") + 9;
        int chEnd = topicStr.indexOf("/", chStart);
        if (chEnd > chStart) {
            uint8_t ch = topicStr.substring(chStart, chEnd).toInt();
            if (ch >= 1 && ch <= MAX_CHANNELS) {
                handleChannelDurationSet(ch, message);
                return;
            }
        }
    }

    // --- Mode select: .../mode/set ---
    if (topicStr.endsWith("/mode/set")) {
        handleModeSet(message);
        return;
    }

    // --- System enable (master switch): .../command ---
    if (topicStr.endsWith("/command")) {
        if (message == "ON") {
            DEBUG_PRINTLN("HomeAssistant: System enabled via MQTT");
            _systemEnabled = true;
            _controller->setSystemEnabled(true);
            if (_currentMode == "disabled") {
                _currentMode = "auto";
                _controller->setManualMode(false);
            }
        } else if (message == "OFF") {
            DEBUG_PRINTLN("HomeAssistant: System disabled via MQTT");
            _systemEnabled = false;
            _controller->setSystemEnabled(false);
            _currentMode = "disabled";
        }
        publishState();
        publishModeState();
        publishChannelStates();
        publishIndividualStatus();
        return;
    }

    // --- Global duration set ---
    if (topicStr.endsWith("/duration/set")) {
        int duration = message.toInt();
        if (duration >= MIN_DURATION_MINUTES && duration <= MAX_DURATION_MINUTES) {
            DEBUG_PRINTF("HomeAssistant: Setting global duration to %d minutes\n", duration);
            for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
                _channelDuration[i] = duration;
            }
            // Publish state back
            String durTopic = buildTopic("duration");
            _mqttClient->publish(durTopic.c_str(), String(duration).c_str(), true);
        }
        return;
    }

    // --- Schedule skip ---
    if (topicStr.endsWith("/schedule/skip")) {
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, message);
        if (err) {
            DEBUG_PRINTF("HomeAssistant: Failed to parse skip payload: %s\n", err.c_str());
            return;
        }

        if (doc["index"].is<const char*>() && String(doc["index"].as<const char*>()) == "all") {
            for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
                _controller->skipSchedule(i);
                if (_nodeManager) {
                    IrrigationSchedule sched = _controller->getSchedule(i);
                    if (sched.channel > NUM_LOCAL_CHANNELS) {
                        _nodeManager->sendSkipToSlave(sched.channel, i);
                    }
                }
            }
        } else {
            uint8_t idx = doc["index"] | 255;
            if (idx < MAX_SCHEDULES) {
                _controller->skipSchedule(idx);
                if (_nodeManager) {
                    IrrigationSchedule sched = _controller->getSchedule(idx);
                    if (sched.channel > NUM_LOCAL_CHANNELS) {
                        _nodeManager->sendSkipToSlave(sched.channel, idx);
                    }
                }
            }
        }
        publishSchedule();
        publishStatus();
        return;
    }

    // --- Schedule unskip ---
    if (topicStr.endsWith("/schedule/unskip")) {
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, message);
        if (err) {
            DEBUG_PRINTF("HomeAssistant: Failed to parse unskip payload: %s\n", err.c_str());
            return;
        }

        if (doc["index"].is<const char*>() && String(doc["index"].as<const char*>()) == "all") {
            for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
                _controller->unskipSchedule(i);
                if (_nodeManager) {
                    IrrigationSchedule sched = _controller->getSchedule(i);
                    if (sched.channel > NUM_LOCAL_CHANNELS) {
                        _nodeManager->sendUnskipToSlave(sched.channel, i);
                    }
                }
            }
        } else {
            uint8_t idx = doc["index"] | 255;
            if (idx < MAX_SCHEDULES) {
                _controller->unskipSchedule(idx);
                if (_nodeManager) {
                    IrrigationSchedule sched = _controller->getSchedule(idx);
                    if (sched.channel > NUM_LOCAL_CHANNELS) {
                        _nodeManager->sendUnskipToSlave(sched.channel, idx);
                    }
                }
            }
        }
        publishSchedule();
        publishStatus();
        return;
    }

    // --- Schedule set (create or update) ---
    if (topicStr.endsWith("/schedule/set")) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, message);
        if (err) {
            DEBUG_PRINTF("HomeAssistant: Failed to parse schedule/set payload: %s\n", err.c_str());
            return;
        }

        uint8_t channel = doc["channel"] | 1;
        uint8_t hour = doc["hour"] | 0;
        uint8_t minute = doc["minute"] | 0;
        uint16_t duration = doc["duration"] | DEFAULT_DURATION_MINUTES;

        // Accept either "weekdays" bitmask or "days" array
        uint8_t weekdays;
        if (doc.containsKey("days") && doc["days"].is<JsonArray>()) {
            weekdays = parseDaysArray(doc["days"].as<JsonArray>());
        } else {
            weekdays = doc["weekdays"] | 0x7F;
        }

        int16_t editIndex = doc["index"] | -1;

        if (channel < 1 || channel > MAX_CHANNELS) {
            DEBUG_PRINTLN("HomeAssistant: schedule/set invalid channel");
            return;
        }
        if (hour > 23 || minute > 59) {
            DEBUG_PRINTLN("HomeAssistant: schedule/set invalid time");
            return;
        }
        if (duration < MIN_DURATION_MINUTES || duration > MAX_DURATION_MINUTES) {
            DEBUG_PRINTLN("HomeAssistant: schedule/set invalid duration");
            return;
        }

        bool ok = false;
        if (editIndex >= 0) {
            ok = _controller->updateSchedule((uint8_t)editIndex, channel, hour, minute, duration, weekdays);
            DEBUG_PRINTF("HomeAssistant: Updated schedule %d via MQTT: ch%d %02d:%02d %dmin -> %s\n",
                         editIndex, channel, hour, minute, duration, ok ? "OK" : "FAIL");
        } else {
            int8_t newIdx = _controller->addSchedule(channel, hour, minute, duration, weekdays);
            ok = (newIdx >= 0);
            DEBUG_PRINTF("HomeAssistant: Added schedule via MQTT: ch%d %02d:%02d %dmin -> idx %d\n",
                         channel, hour, minute, duration, newIdx);
        }

        // Sync to slave if virtual channel
        if (ok && _nodeManager && channel > NUM_LOCAL_CHANNELS) {
            for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                const NodePeer* slave = _nodeManager->getSlave(s);
                if (slave && channel >= slave->base_virtual_ch &&
                    channel < slave->base_virtual_ch + slave->num_channels) {
                    _nodeManager->sendScheduleSync(slave->node_id);
                    break;
                }
            }
        }

        publishSchedule();
        publishStatus();
        return;
    }

    // --- Schedule delete ---
    if (topicStr.endsWith("/schedule/delete")) {
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, message);
        if (err) {
            DEBUG_PRINTF("HomeAssistant: Failed to parse schedule/delete payload: %s\n", err.c_str());
            return;
        }

        uint8_t idx = doc["index"] | 255;
        if (idx >= MAX_SCHEDULES) {
            DEBUG_PRINTLN("HomeAssistant: schedule/delete invalid index");
            return;
        }

        IrrigationSchedule sched = _controller->getSchedule(idx);
        uint8_t schedCh = sched.channel;

        if (_controller->removeSchedule(idx)) {
            DEBUG_PRINTF("HomeAssistant: Deleted schedule %d via MQTT\n", idx);

            if (_nodeManager && schedCh > NUM_LOCAL_CHANNELS) {
                for (uint8_t s = 0; s < _nodeManager->getSlaveCount(); s++) {
                    const NodePeer* slave = _nodeManager->getSlave(s);
                    if (slave && schedCh >= slave->base_virtual_ch &&
                        schedCh < slave->base_virtual_ch + slave->num_channels) {
                        _nodeManager->sendScheduleSync(slave->node_id);
                        break;
                    }
                }
            }
        } else {
            DEBUG_PRINTF("HomeAssistant: Failed to delete schedule %d\n", idx);
        }

        publishSchedule();
        publishStatus();
        return;
    }
}

// ============================================================================
// Per-Channel Handlers
// ============================================================================

void HomeAssistantIntegration::handleChannelCommand(uint8_t channel, const String& message) {
    String chBase = "channel/" + String(channel);
    uint8_t idx = channel - 1;

    if (message == "ON") {
        DEBUG_PRINTF("HomeAssistant: Channel %d ON via MQTT\n", channel);

        if (!_systemEnabled) {
            DEBUG_PRINTLN("HomeAssistant: System disabled, ignoring channel ON");
            // Publish state back as OFF
            _mqttClient->publish(buildTopic((chBase + "/state").c_str()).c_str(), "OFF", true);
            return;
        }

        _controller->startIrrigation(channel, _channelDuration[idx]);

        // Set retry flag for reconciliation
        _retryPending[idx] = true;
        _commandSentTime[idx] = millis();

    } else if (message == "OFF") {
        DEBUG_PRINTF("HomeAssistant: Channel %d OFF via MQTT\n", channel);
        _controller->stopIrrigation(channel);
        _retryPending[idx] = false;
        _commandSentTime[idx] = 0;
    }

    // Immediate publish
    publishChannelStates();
    publishIndividualStatus();
    publishState();
}

void HomeAssistantIntegration::handleChannelDurationSet(uint8_t channel, const String& message) {
    int duration = message.toInt();
    if (duration >= MIN_DURATION_MINUTES && duration <= MAX_DURATION_MINUTES) {
        uint8_t idx = channel - 1;
        _channelDuration[idx] = duration;
        DEBUG_PRINTF("HomeAssistant: Channel %d duration set to %d minutes\n", channel, duration);

        // Publish state back
        String topic = buildTopic(("channel/" + String(channel) + "/duration").c_str());
        _mqttClient->publish(topic.c_str(), String(duration).c_str(), true);
    }
}

void HomeAssistantIntegration::handleModeSet(const String& message) {
    DEBUG_PRINTF("HomeAssistant: Mode set to %s via MQTT\n", message.c_str());

    if (message == "auto") {
        _currentMode = "auto";
        _systemEnabled = true;
        _controller->setSystemEnabled(true);
        _controller->setManualMode(false);
    } else if (message == "manual") {
        _currentMode = "manual";
        _systemEnabled = true;
        _controller->setSystemEnabled(true);
        _controller->setManualMode(true);
    } else if (message == "disabled") {
        _currentMode = "disabled";
        _systemEnabled = false;
        _controller->setSystemEnabled(false);
    } else {
        DEBUG_PRINTF("HomeAssistant: Unknown mode '%s'\n", message.c_str());
        return;
    }

    publishModeState();
    publishState();
    publishChannelStates();
    publishIndividualStatus();
}

void HomeAssistantIntegration::publishModeState() {
    if (!isConnected()) return;
    String topic = buildTopic("mode");
    _mqttClient->publish(topic.c_str(), _currentMode.c_str(), true);
}

// ============================================================================
// Publishing Methods
// ============================================================================

void HomeAssistantIntegration::publishState() {
    if (!isConnected()) return;

    String topic = buildTopic("state");
    String payload = _systemEnabled ? "ON" : "OFF";
    _mqttClient->publish(topic.c_str(), payload.c_str(), true);
}

void HomeAssistantIntegration::publishStatus() {
    if (!isConnected()) return;

    SystemStatus status = _controller->getStatus();
    StaticJsonDocument<512> doc;

    doc["irrigating"] = status.irrigating;
    doc["manual_mode"] = status.manualMode;
    doc["wifi_connected"] = status.wifiConnected;
    doc["mqtt_connected"] = status.mqttConnected;
    doc["system_enabled"] = _systemEnabled;
    doc["mode"] = _currentMode;

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

    // Skipped schedules summary
    String skippedStr;
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        if (_controller->isScheduleSkipped(i)) {
            if (skippedStr.length() > 0) skippedStr += ",";
            skippedStr += String(i);
        }
    }
    doc["skipped_schedules"] = skippedStr.length() > 0 ? skippedStr : "none";

    String jsonString;
    serializeJson(doc, jsonString);

    String topic = buildTopic("status");
    _mqttClient->publish(topic.c_str(), jsonString.c_str(), true);
}

void HomeAssistantIntegration::publishIndividualStatus() {
    if (!isConnected()) return;

    SystemStatus status = _controller->getStatus();

    // Irrigating
    String irrigTopic = buildTopic("status/irrigating");
    _mqttClient->publish(irrigTopic.c_str(), status.irrigating ? "true" : "false", true);

    // Time remaining (global — max across active channels)
    unsigned long globalRemaining = 0;
    for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
        unsigned long chRemaining = getChannelTimeRemaining(ch);
        if (chRemaining > globalRemaining) {
            globalRemaining = chRemaining;
        }
    }
    String trTopic = buildTopic("status/time_remaining");
    _mqttClient->publish(trTopic.c_str(), String(globalRemaining).c_str(), true);

    // Next scheduled (ISO8601 UTC)
    unsigned long nextTime = _controller->getNextScheduledTime();
    String nsTopic = buildTopic("status/next_scheduled");
    if (nextTime > 0) {
        _mqttClient->publish(nsTopic.c_str(), toISO8601((time_t)nextTime).c_str(), true);
    } else {
        _mqttClient->publish(nsTopic.c_str(), "unknown", true);
    }

    // System enabled
    String seTopic = buildTopic("status/system_enabled");
    _mqttClient->publish(seTopic.c_str(), _systemEnabled ? "ON" : "OFF", true);
}

void HomeAssistantIntegration::publishChannelStates() {
    if (!isConnected()) return;

    for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
        if (!_controller->isChannelEnabled(ch)) continue;

        String chBase = "channel/" + String(ch);
        uint8_t idx = ch - 1;

        // Channel state (switch entity — reflects last command intent)
        bool running = _controller->isChannelIrrigating(ch);
        String stateTopic = buildTopic((chBase + "/state").c_str());
        _mqttClient->publish(stateTopic.c_str(), running ? "ON" : "OFF", true);

        // Channel running (binary_sensor — actual execution)
        String runTopic = buildTopic((chBase + "/running").c_str());
        _mqttClient->publish(runTopic.c_str(), running ? "true" : "false", true);

        // Channel time remaining
        unsigned long remaining = getChannelTimeRemaining(ch);
        String trTopic = buildTopic((chBase + "/time_remaining").c_str());
        _mqttClient->publish(trTopic.c_str(), String(remaining).c_str(), true);
    }
}

void HomeAssistantIntegration::publishSchedule() {
    if (!isConnected()) return;

    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count;
    _controller->getSchedules(schedules, count);

    DynamicJsonDocument doc(1536);
    JsonArray array = doc.createNestedArray("schedules");

    for (int i = 0; i < count; i++) {
        if (schedules[i].enabled) {
            JsonObject schedule = array.createNestedObject();
            schedule["index"] = i;
            schedule["channel"] = schedules[i].channel;
            schedule["hour"] = schedules[i].hour;
            schedule["minute"] = schedules[i].minute;
            schedule["duration"] = schedules[i].durationMinutes;
            schedule["weekdays"] = schedules[i].weekdays;
            schedule["days"] = serialized(weekdaysToDaysArray(schedules[i].weekdays));
            schedule["skipped"] = _controller->isScheduleSkipped(i);
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);

    String topic = buildTopic("schedules");
    _mqttClient->publish(topic.c_str(), jsonString.c_str(), true);
}

// ============================================================================
// Utility Methods
// ============================================================================

String HomeAssistantIntegration::buildTopic(const char* suffix) {
    return String(MQTT_BASE_TOPIC) + "/" + suffix;
}

String HomeAssistantIntegration::toISO8601(time_t t) {
    struct tm timeinfo;
    gmtime_r(&t, &timeinfo);
    char buf[25];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
}

uint8_t HomeAssistantIntegration::parseDaysArray(JsonArray days) {
    uint8_t bitmask = 0;
    for (const char* day : days) {
        String d = String(day);
        d.toLowerCase();
        if (d == "sun") bitmask |= (1 << 0);
        else if (d == "mon") bitmask |= (1 << 1);
        else if (d == "tue") bitmask |= (1 << 2);
        else if (d == "wed") bitmask |= (1 << 3);
        else if (d == "thu") bitmask |= (1 << 4);
        else if (d == "fri") bitmask |= (1 << 5);
        else if (d == "sat") bitmask |= (1 << 6);
    }
    return bitmask;
}

String HomeAssistantIntegration::weekdaysToDaysArray(uint8_t weekdays) {
    const char* dayNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    String result = "[";
    bool first = true;
    for (uint8_t i = 0; i < 7; i++) {
        if (weekdays & (1 << i)) {
            if (!first) result += ",";
            result += "\"";
            result += dayNames[i];
            result += "\"";
            first = false;
        }
    }
    result += "]";
    return result;
}

unsigned long HomeAssistantIntegration::getChannelTimeRemaining(uint8_t channel) {
    if (channel < 1 || channel > MAX_CHANNELS) return 0;
    uint8_t idx = channel - 1;

    SystemStatus status = _controller->getStatus();
    if (!status.channelIrrigating[idx]) return 0;
    if (status.channelStartTime[idx] == 0) return 0;

    unsigned long elapsedMs = millis() - status.channelStartTime[idx];
    unsigned long durationMs = (unsigned long)status.channelDuration[idx] * 60000UL;

    if (elapsedMs >= durationMs) return 0;
    return (durationMs - elapsedMs) / 1000;  // Return seconds
}
