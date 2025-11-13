/*
 * ESP32 Single-Channel Irrigation Controller
 *
 * Features:
 * - Single valve/pump control via relay or MOSFET
 * - Schedule-based irrigation with offline operation
 * - LCD display with button controls
 * - WiFi connectivity with auto-reconnect
 * - NTP time synchronization with RTC fallback
 * - OTA updates from GitHub
 * - Home Assistant MQTT integration with auto-discovery
 * - Non-volatile storage for schedules (SPIFFS)
 *
 * Author: Your Name
 * Version: 1.0.0
 */

#include <Arduino.h>
#include "Config.h"
#include "IrrigationController.h"
#include "DisplayManager.h"
#include "WiFiManager.h"
#include "HomeAssistantIntegration.h"

// Global objects
IrrigationController* irrigationController = nullptr;
DisplayManager* displayManager = nullptr;
WiFiManager* wifiManager = nullptr;
HomeAssistantIntegration* homeAssistant = nullptr;

// System status
unsigned long lastStatusUpdate = 0;

// Function prototypes
void timeUpdateCallback(time_t currentTime);
void updateSystemStatus();
void loadConfiguration();

void setup() {
    // Initialize serial for debugging
#if ENABLE_SERIAL_DEBUG
    Serial.begin(SERIAL_BAUD_RATE);
    while (!Serial && millis() < 3000); // Wait up to 3 seconds for serial
    DEBUG_PRINTLN("\n\n==================================");
    DEBUG_PRINTLN("ESP32 Irrigation Controller");
    DEBUG_PRINTLN("Version: " VERSION);
    DEBUG_PRINTLN("==================================\n");
#endif

    // Initialize LEDs
    pinMode(LED_STATUS, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    digitalWrite(LED_STATUS, HIGH); // Turn on red LED (power indicator)
    digitalWrite(LED_BLUE, LOW);    // Blue LED off initially

    // Load configuration from SPIFFS
    loadConfiguration();

    // Initialize irrigation controller
    DEBUG_PRINTLN("Initializing Irrigation Controller...");
    irrigationController = new IrrigationController();
    if (!irrigationController->begin()) {
        DEBUG_PRINTLN("ERROR: Failed to initialize IrrigationController!");
    }

    // Initialize display manager
    DEBUG_PRINTLN("Initializing Display Manager...");
    displayManager = new DisplayManager(irrigationController);
    if (!displayManager->begin()) {
        DEBUG_PRINTLN("ERROR: Failed to initialize DisplayManager!");
    } else {
        displayManager->showMessage("Irrigation System", "Starting up...", "", "");
        delay(1000);
    }

    // Initialize WiFi manager
    DEBUG_PRINTLN("Initializing WiFi Manager...");
    wifiManager = new WiFiManager();
    wifiManager->setController(irrigationController);

    // Try to connect with saved credentials or start config portal
    if (!wifiManager->begin()) {
        DEBUG_PRINTLN("WiFiManager: Started in configuration mode");
        displayManager->showMessage("WiFi Setup Mode", "Connect to:", WIFI_AP_NAME, "to configure WiFi");
    } else {
        DEBUG_PRINTLN("WiFiManager: Connected successfully");
    }

    // Set time update callback
    wifiManager->setTimeUpdateCallback(timeUpdateCallback);

    // Initialize Home Assistant integration
    DEBUG_PRINTLN("Initializing Home Assistant Integration...");
    homeAssistant = new HomeAssistantIntegration(irrigationController);
    if (!homeAssistant->begin(MQTT_BROKER, MQTT_PORT, MQTT_USER, MQTT_PASSWORD)) {
        DEBUG_PRINTLN("ERROR: Failed to initialize HomeAssistant!");
    }

    // Add some default schedules for testing (optional)
    // Schedule 1: Every day at 6:00 AM for 30 minutes
    irrigationController->addSchedule(0, 6, 0, 30, 0x7F); // 0x7F = all days

    // Schedule 2: Every day at 6:00 PM for 20 minutes
    irrigationController->addSchedule(1, 18, 0, 20, 0x7F);

    // Show initial status
    displayManager->showStatus();

    // Turn off LED - boot complete
    digitalWrite(LED_STATUS, LOW);

    DEBUG_PRINTLN("\n==================================");
    DEBUG_PRINTLN("System initialized successfully!");
    DEBUG_PRINTLN("==================================\n");
}

void loop() {
    // Update all components
    irrigationController->update();
    displayManager->update();
    wifiManager->update();
    homeAssistant->update();

    // Update system status periodically
    unsigned long currentMillis = millis();
    if (currentMillis - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
        lastStatusUpdate = currentMillis;
        updateSystemStatus();
    }

    // Flash blue LED if irrigating
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
    status.mqttConnected = homeAssistant->isConnected();

    // Update current time if WiFi is connected
    if (status.wifiConnected && wifiManager->isTimeSynced()) {
        irrigationController->setCurrentTime(wifiManager->getCurrentTime());
    }

    DEBUG_PRINTF("Status - WiFi: %s, MQTT: %s, Irrigating: %s\n",
                 status.wifiConnected ? "OK" : "NO",
                 status.mqttConnected ? "OK" : "NO",
                 status.irrigating ? "YES" : "NO");
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

    // Check if config file exists
    if (!SPIFFS.exists(CONFIG_FILE)) {
        DEBUG_PRINTLN("No configuration file found, using defaults");
        return;
    }

    // Load config file
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("Failed to open config file");
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("Failed to parse config file: %s\n", error.c_str());
        return;
    }

    // Read configuration values
    // You can add custom configuration loading here
    // For example:
    // String ssid = doc["wifi"]["ssid"] | WIFI_SSID;
    // String password = doc["wifi"]["password"] | WIFI_PASSWORD;

    DEBUG_PRINTLN("Configuration loaded successfully");
}
