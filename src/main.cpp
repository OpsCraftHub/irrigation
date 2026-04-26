/*
 * ESP32 Multi-Node Irrigation Controller
 *
 * Features:
 * - Multi-channel valve control via relay or MOSFET
 * - Schedule-based irrigation with offline operation
 * - LCD display with button controls (Board A)
 * - Status LED for headless operation (Board B)
 * - WiFi connectivity with auto-reconnect
 * - NTP time synchronization with RTC fallback
 * - Automatic OTA updates from GitHub
 * - Home Assistant MQTT integration with auto-discovery
 * - Non-volatile storage for schedules (SPIFFS)
 * - Runtime feature flags from config.json
 */


#include <Arduino.h>
#include <esp_mac.h>
#include "Config.h"
#include "IrrigationController.h"
#include "DisplayManager.h"
#include "WiFiManager.h"
#include "HomeAssistantIntegration.h"
#include "NodeManager.h"

// Global objects
IrrigationController* irrigationController = nullptr;
DisplayManager* displayManager = nullptr;
WiFiManager* wifiManager = nullptr;
HomeAssistantIntegration* homeAssistant = nullptr;
NodeManager* nodeManager = nullptr;

// Feature flags — multi_node on by default for both boards
//                  {multi_node, mqtt, web_ui, sensors, battery, ota, debug}
Features features = {true, false, true, false, false, true, false};

// Node identity
String nodeId = DEFAULT_NODE_ID;
#ifdef BOARD_B
String nodeRole = "slave";
#else
String nodeRole = DEFAULT_ROLE;
#endif
String nodeName = "Slave";  // Human-readable name for pairing

// System status
unsigned long lastStatusUpdate = 0;

// Function prototypes
void timeUpdateCallback(time_t currentTime);
void updateSystemStatus();
void loadConfiguration();
void remoteValveHandler(uint8_t channel, bool state, uint16_t duration);
void onPairRequest(const char* nodeId, const char* name);
void onPairResponse(bool accepted);
String nodeIdToDisplayName(const String& id);

void setup() {
    // Initialize serial for debugging
#if ENABLE_SERIAL_DEBUG
    Serial.begin(SERIAL_BAUD_RATE);
#ifdef BOARD_B
    // C3 USB-CDC drops connection on reset — need longer wait for re-enumeration
    delay(3000);
#else
    while (!Serial && millis() < 3000);
#endif
    DEBUG_PRINTLN("\n\n==================================");
    DEBUG_PRINTLN("ESP32 Irrigation Controller");
    DEBUG_PRINTLN("Version: " VERSION);
    DEBUG_PRINTF("MAC: %s\n", WiFi.macAddress().c_str());
#ifdef BOARD_B
    DEBUG_PRINTLN("Board: B (ESP32-C3)");
#else
    DEBUG_PRINTLN("Board: A (ESP32)");
#endif
    DEBUG_PRINTLN("==================================\n");
#endif

    // Initialize LEDs (guard against pin -1)
    if (LED_STATUS >= 0) {
        pinMode(LED_STATUS, OUTPUT);
        digitalWrite(LED_STATUS, HIGH); // Turn on LED (power indicator)
    }
    if (LED_BLUE >= 0) {
        pinMode(LED_BLUE, OUTPUT);
        digitalWrite(LED_BLUE, LOW);    // Blue LED off initially
    }

    // Load configuration from SPIFFS (including feature flags)
    loadConfiguration();

    // Initialize irrigation controller — always init (core function)
    DEBUG_PRINTLN("Initializing Irrigation Controller...");
    irrigationController = new IrrigationController();
    if (!irrigationController->begin()) {
        DEBUG_PRINTLN("ERROR: Failed to initialize IrrigationController!");
    }

    // Initialize display manager — skip if no LCD pins defined
#if LCD_ROWS > 0
    DEBUG_PRINTLN("Initializing Display Manager...");
    displayManager = new DisplayManager(irrigationController);
    if (!displayManager->begin()) {
        DEBUG_PRINTLN("WARNING: Display init failed");
    } else {
        displayManager->showMessage("Irrigation System", "Starting up...", "", "");
        delay(1000);
    }
#else
    DEBUG_PRINTLN("No LCD configured, skipping DisplayManager");
#endif

    // Initialize Home Assistant integration — only if mqtt feature enabled
    if (features.mqtt) {
        DEBUG_PRINTLN("Initializing Home Assistant Integration...");
        homeAssistant = new HomeAssistantIntegration(irrigationController);
        homeAssistant->begin();
    } else {
        DEBUG_PRINTLN("MQTT feature disabled, skipping HomeAssistant");
    }

    // Initialize WiFi manager — always init (needed for NTP, OTA, web)
    DEBUG_PRINTLN("Initializing WiFi Manager...");
    wifiManager = new WiFiManager();
    wifiManager->setController(irrigationController);
    if (features.mqtt && homeAssistant) {
        wifiManager->setHomeAssistant(homeAssistant);
    }

    // Try to connect with saved credentials or start config portal
    if (!wifiManager->begin()) {
        DEBUG_PRINTLN("WiFiManager: Started in configuration mode");
#if LCD_ROWS > 0
        if (displayManager) {
            displayManager->showMessage("WiFi Setup Mode", "Connect to:", WIFI_AP_NAME, "to configure WiFi");
        }
#endif
    } else {
        DEBUG_PRINTLN("WiFiManager: Connected successfully");
        DEBUG_PRINTF("IP Address: %s\n", WiFi.localIP().toString().c_str());
        DEBUG_PRINTF("MAC Address: %s\n", WiFi.macAddress().c_str());
#if LCD_ROWS > 0
        if (displayManager) {
            // Flash IP address on screen for 3 seconds
            String ipLine = WiFi.localIP().toString();
            displayManager->showMessage("WiFi Connected!", ipLine.c_str(), "", "");
            delay(3000);
        }
#endif
    }

    // Set time update callback
    wifiManager->setTimeUpdateCallback(timeUpdateCallback);

    // Initialize NodeManager (UDP + mDNS) — only if multi_node feature enabled
    if (features.multi_node) {
        DEBUG_PRINTLN("Initializing NodeManager (UDP + mDNS)...");
        nodeManager = new NodeManager();
        nodeManager->setController(irrigationController);
        nodeManager->setNodeId(nodeId.c_str());

        if (nodeRole == "master") {
            nodeManager->setRole(NODE_ROLE_MASTER);
        } else {
            nodeManager->setRole(NODE_ROLE_SLAVE);
            nodeManager->setNodeName(nodeName.c_str());
        }

        if (nodeManager->begin()) {
            if (nodeRole == "master") {
                // Master: set remote valve callback and pairing callback
                irrigationController->setRemoteValveCallback(remoteValveHandler);
                nodeManager->setPairRequestCallback(onPairRequest);
                wifiManager->setNodeManager(nodeManager);
                if (features.mqtt && homeAssistant) {
                    homeAssistant->setNodeManager(nodeManager);
                }
#if LCD_ROWS > 0
                if (displayManager) {
                    displayManager->setPairResponseCallback(onPairResponse);
                }
#endif
            }
        } else {
            DEBUG_PRINTLN("ERROR: NodeManager init failed");
            delete nodeManager;
            nodeManager = nullptr;
        }
    } else {
        DEBUG_PRINTLN("multi_node feature disabled, skipping NodeManager");
    }

    // Show initial status
#if LCD_ROWS > 0
    if (displayManager) {
        displayManager->showStatus();
    }
#endif

    // Turn off LED - boot complete
    if (LED_STATUS >= 0) {
        digitalWrite(LED_STATUS, LOW);
    }

    DEBUG_PRINTLN("\n==================================");
    DEBUG_PRINTLN("System initialized successfully!");
    DEBUG_PRINTLN("Version: " VERSION);
    DEBUG_PRINTF("MAC: %s\n", WiFi.macAddress().c_str());
    if (wifiManager->isConnected()) {
        DEBUG_PRINTF("IP: %s\n", WiFi.localIP().toString().c_str());
    }
    DEBUG_PRINTF("Features: mqtt=%d web_ui=%d ota=%d debug=%d\n",
                 features.mqtt, features.web_ui, features.ota, features.debug);
    DEBUG_PRINTLN("==================================\n");
}

void loop() {
    // Update all components
    irrigationController->update();

#if LCD_ROWS > 0
    if (displayManager) displayManager->update();
#endif

    wifiManager->update();

    if (features.mqtt && homeAssistant) homeAssistant->update();

    if (features.multi_node && nodeManager) nodeManager->update();

    // Update system status periodically
    unsigned long currentMillis = millis();
    if (currentMillis - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
        lastStatusUpdate = currentMillis;
        updateSystemStatus();
    }

    // Flash blue LED if irrigating (Board A only)
    if (LED_BLUE >= 0) {
        if (irrigationController->isIrrigating()) {
            static unsigned long lastBlink = 0;
            static bool ledState = false;
            if (currentMillis - lastBlink >= 500) {
                lastBlink = currentMillis;
                ledState = !ledState;
                digitalWrite(LED_BLUE, ledState);
            }
        } else {
            digitalWrite(LED_BLUE, LOW);
        }
    }

#if LCD_ROWS > 0
    // Debug toggle: hold NEXT + SELECT for 3s (Board A only)
    static unsigned long debugComboStart = 0;
    if (digitalRead(BTN_NEXT) == LOW && digitalRead(BTN_SELECT) == LOW) {
        if (debugComboStart == 0) debugComboStart = millis();
        if (millis() - debugComboStart > 3000) {
            features.debug = !features.debug;
            DEBUG_PRINTF("Debug mode %s\n", features.debug ? "ON" : "OFF");
            debugComboStart = 0;
            delay(500); // debounce
        }
    } else {
        debugComboStart = 0;
    }
#endif

    // Small delay to prevent watchdog timeout
    delay(10);
}

void timeUpdateCallback(time_t currentTime) {
    DEBUG_PRINTF("Time updated: %lu\n", currentTime);
    irrigationController->setCurrentTime(currentTime);
}

void updateSystemStatus() {
    SystemStatus status = irrigationController->getStatus();

    // Update WiFi status
    status.wifiConnected = wifiManager->isConnected();

    // Update MQTT status
    if (features.mqtt && homeAssistant) {
        status.mqttConnected = homeAssistant->isConnected();
    } else {
        status.mqttConnected = false;
    }

    // Update current time if WiFi is connected
    if (status.wifiConnected && wifiManager->isTimeSynced()) {
        irrigationController->setCurrentTime(wifiManager->getCurrentTime());
    }

    DEBUG_PRINTF("Status - WiFi: %s, MQTT: %s, Irrigating: %s\n",
                 status.wifiConnected ? "OK" : "NO",
                 status.mqttConnected ? "OK" : "NO",
                 status.irrigating ? "YES" : "NO");
}

void remoteValveHandler(uint8_t channel, bool state, uint16_t duration) {
    if (!nodeManager) return;
    if (state) {
        nodeManager->sendStart(channel, duration);
    } else {
        nodeManager->sendStop(channel);
    }
}

void onPairRequest(const char* nodeId, const char* name) {
    DEBUG_PRINTF("Pair request from '%s' (%s)\n", name, nodeId);
#if LCD_ROWS > 0
    if (displayManager) {
        displayManager->showPairRequest(nodeId, name);
    }
#endif
    // Web UI polls /api/nodes/pending automatically
}

void onPairResponse(bool accepted) {
    if (!nodeManager) return;
    if (accepted) {
        DEBUG_PRINTLN("User accepted pair request");
        const PendingPairRequest& pending = nodeManager->getPendingPair();
        char newSlaveId[12];
        strncpy(newSlaveId, pending.node_id, sizeof(newSlaveId) - 1);
        newSlaveId[sizeof(newSlaveId) - 1] = '\0';

        nodeManager->acceptPendingPair();

        // Push all relevant schedules to the newly paired slave
        nodeManager->sendScheduleSync(newSlaveId);

#if LCD_ROWS > 0
        if (displayManager) {
            displayManager->clearPairRequest();
        }
#endif
    } else {
        DEBUG_PRINTLN("User rejected pair request");
        nodeManager->rejectPendingPair(PAIR_REJECT_USER);
    }
}

// Convert node_id like "far_tap" to display name "Far Tap"
String nodeIdToDisplayName(const String& id) {
    String name = id;
    name.replace('_', ' ');
    bool capNext = true;
    for (unsigned int i = 0; i < name.length(); i++) {
        if (name[i] == ' ') {
            capNext = true;
        } else if (capNext) {
            name[i] = toupper(name[i]);
            capNext = false;
        }
    }
    return name;
}

void loadConfiguration() {
    DEBUG_PRINTLN("Loading configuration from SPIFFS...");

    // Initialize SPIFFS (format if needed)
    DEBUG_PRINTLN("Mounting SPIFFS...");
    if (!SPIFFS.begin(false)) {
        DEBUG_PRINTLN("SPIFFS not formatted, formatting now...");
        if (!SPIFFS.begin(true)) {
            DEBUG_PRINTLN("ERROR: Failed to format/mount SPIFFS");
            return;
        }
        DEBUG_PRINTLN("SPIFFS formatted successfully");
    }

    // Auto-generate unique node_id from MAC if no config exists
    if (!SPIFFS.exists(CONFIG_FILE)) {
        DEBUG_PRINTLN("No configuration file found, using defaults");
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        char autoId[12];
        snprintf(autoId, sizeof(autoId), "node_%02x%02x", mac[4], mac[5]);
        nodeId = autoId;
        nodeName = nodeIdToDisplayName(nodeId);
        DEBUG_PRINTF("Auto-generated node_id: %s (%s)\n", nodeId.c_str(), nodeName.c_str());
        return;
    }

    // Load config file
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("Failed to open config file");
        return;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("Failed to parse config file: %s\n", error.c_str());
        return;
    }

    // Read node identity
    nodeId = doc["node_id"] | DEFAULT_NODE_ID;
    nodeRole = doc["role"] | DEFAULT_ROLE;

    // If node_id is default, auto-generate a unique one from MAC
    if (nodeId == DEFAULT_NODE_ID) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        char autoId[12];
        snprintf(autoId, sizeof(autoId), "node_%02x%02x", mac[4], mac[5]);
        nodeId = autoId;
        DEBUG_PRINTF("Auto-generated node_id: %s\n", nodeId.c_str());
    }

    // Display name: explicit node_name wins, otherwise derive from node_id
    const char* explicitName = doc["node_name"] | "";
    nodeName = (strlen(explicitName) > 0) ? String(explicitName) : nodeIdToDisplayName(nodeId);

    // Read feature flags
    JsonObject feat = doc["features"];
    if (!feat.isNull()) {
        features.multi_node = feat["multi_node"] | false;
        features.mqtt = feat["mqtt"] | false;
        features.web_ui = feat["web_ui"] | true;
        features.sensors = feat["sensors"] | false;
        features.battery = feat["battery"] | false;
        features.ota = feat["ota"] | true;
        features.debug = feat["debug"] | false;
    }

    DEBUG_PRINTLN("Configuration loaded successfully");
    DEBUG_PRINTF("Node: %s, Role: %s, Name: %s\n", nodeId.c_str(), nodeRole.c_str(), nodeName.c_str());
}
