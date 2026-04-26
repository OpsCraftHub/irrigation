#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// FIRMWARE VERSION
// ============================================================================
#ifndef VERSION
#define VERSION "1.0.38"
#endif

// ============================================================================
// MULTI-NODE SETTINGS
// ============================================================================

#define MAX_SLAVES 8  // Max slave nodes a master can track

// ============================================================================
// PIN DEFINITIONS (Board-Aware)
// ============================================================================

#ifdef BOARD_B
    // ESP32-C3 (Board B slave) — single channel, no LCD, no buttons
    #define MAX_CHANNELS 1
    #define NUM_LOCAL_CHANNELS 1
    #define CHANNEL_1_PIN 5

    // No buttons on Board B
    #define BTN_START -1
    #define BTN_STOP -1
    #define BTN_NEXT -1
    #define BTN_SELECT -1

    // No LCD on Board B
    #define LCD_ADDRESS 0x27
    #define LCD_COLS 0
    #define LCD_ROWS 0
    #define LCD_SDA -1
    #define LCD_SCL -1

    // Status LED on C3 (morse-style patterns for headless debugging)
    #define LED_STATUS 3
    #define LED_BLUE -1
    #define STATUS_LED_PIN 3
#else
    // ESP32 (Board A master) — multi-channel, LCD, buttons
    #define MAX_CHANNELS (NUM_LOCAL_CHANNELS + MAX_SLAVES)  // 6 local + up to 4 virtual
    #define NUM_LOCAL_CHANNELS 6    // PCB has 6 MOSFET channels
    #define CHANNEL_1_PIN 25  // GPIO25 - Channel 1 (wired)
    #define CHANNEL_2_PIN 4   // GPIO4  - Channel 2 (wired)
    #define CHANNEL_3_PIN 14  // GPIO14 - Channel 3 (not yet wired)
    #define CHANNEL_4_PIN 16  // GPIO16 - Channel 4 (not yet wired)
    #define CHANNEL_5_PIN 17  // GPIO17 - Channel 5 (not yet wired)
    #define CHANNEL_6_PIN 18  // GPIO18 - Channel 6 (not yet wired)

    // Button Inputs (active LOW with internal pullup)
    #define BTN_START 32   // Start irrigation manually
    #define BTN_STOP 33    // Stop irrigation
    #define BTN_NEXT 26    // Navigate menu (next item)
    #define BTN_SELECT 27  // Select/confirm in menu

    // LCD I2C Configuration
    #define LCD_ADDRESS 0x27  // Common I2C address for LCD (try 0x3F if not working)
    #define LCD_COLS 16       // 16x2 LCD
    #define LCD_ROWS 2
    #define LCD_SDA 21        // I2C SDA pin
    #define LCD_SCL 22        // I2C SCL pin

    // Status LEDs
    #define LED_STATUS 2      // Built-in LED on most ESP32 boards (Red - Power)
    #define LED_BLUE 15       // Blue LED - Irrigation indicator
    #define STATUS_LED_PIN -1 // No dedicated status LED on Board A (has LCD instead)
#endif

// Legacy definition for backward compatibility
#define VALVE_PIN CHANNEL_1_PIN

// ============================================================================
// IRRIGATION SETTINGS
// ============================================================================

#define MAX_SCHEDULES 16             // Maximum number of irrigation schedules (4 per channel)
#define DEFAULT_DURATION_MINUTES 30  // Default irrigation duration
#define MIN_DURATION_MINUTES 1       // Minimum duration
#define MAX_DURATION_MINUTES 240     // Maximum duration (4 hours)

// Safety timeout - automatically stop if irrigation runs too long
#define SAFETY_TIMEOUT_MINUTES 300   // 5 hours maximum

// ============================================================================
// WIFI SETTINGS
// ============================================================================

// These will be overridden by config.json if it exists
#define WIFI_SSID "YourWiFiSSID"
#define WIFI_PASSWORD "YourWiFiPassword"
#define WIFI_HOSTNAME "irrigation-esp32"
#define WIFI_RECONNECT_INTERVAL 30000  // Retry every 30 seconds
#define WIFI_MAX_RETRIES 3             // Try 3 times before giving up temporarily

// WiFi Configuration Portal
#define WIFI_AP_NAME "Irrigation-Setup"     // Access Point name
#define WIFI_AP_PASSWORD ""                 // Leave empty for open network
#define WIFI_CONFIG_PORTAL_TIMEOUT 300000   // 5 minutes timeout
#define WIFI_CREDENTIALS_FILE "/wifi_creds.json"
#define MQTT_CREDENTIALS_FILE "/mqtt_creds.json"
#define DNS_PORT 53

// ============================================================================
// MQTT SETTINGS (Home Assistant)
// ============================================================================

#define MQTT_BROKER "home.hackster.me"    // Your Home Assistant hostname
#define MQTT_PORT 1883
#define MQTT_USER ""                      // MQTT username (UPDATE THIS)
#define MQTT_PASSWORD ""                  // MQTT password (UPDATE THIS)
#define MQTT_CLIENT_ID "irrigation_esp32"
#define MQTT_BASE_TOPIC "homeassistant/switch/irrigation"
#define MQTT_RECONNECT_INTERVAL 5000   // Retry every 5 seconds

// Home Assistant MQTT Discovery
#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_DEVICE_NAME "Irrigation Controller"
#define HA_DEVICE_ID "irrigation_esp32_001"

// ============================================================================
// NTP SETTINGS
// ============================================================================

#define NTP_SERVER "za.pool.ntp.org"
#define NTP_UPDATE_INTERVAL 3600000    // Update every hour (milliseconds)
#define TIMEZONE_OFFSET 2              // SAST = UTC+2 (South Africa Standard Time)
#define DAYLIGHT_OFFSET 0              // South Africa doesn't use DST

// ============================================================================
// OTA UPDATE SETTINGS
// ============================================================================

#define OTA_PASSWORD "irrigation123"   // Password for OTA updates
#define OTA_CHECK_INTERVAL 86400000    // Check daily (24 hours in milliseconds)

// GitHub repository for firmware updates
#define GITHUB_REPO_OWNER "OpsCraftHub"
#define GITHUB_REPO_NAME "irrigation"

#ifdef BOARD_B
    #define GITHUB_FIRMWARE_PATH "firmware/board_b_c3/firmware.bin"
    #define GITHUB_VERSION_PATH "firmware/board_b_c3/version.txt"
#else
    #define GITHUB_FIRMWARE_PATH "firmware/board_a/firmware.bin"
    #define GITHUB_VERSION_PATH "firmware/board_a/version.txt"
#endif

// ============================================================================
// STORAGE SETTINGS
// ============================================================================

#define CONFIG_FILE "/config.json"
#define SCHEDULE_FILE "/schedule.json"
#define LOG_FILE "/irrigation.log"
#define MAX_LOG_ENTRIES 100
#define PAIRED_SLAVES_FILE "/paired_slaves.json"
#define PAIRED_MASTER_FILE "/paired_master.json"

// ============================================================================
// TIMING CONSTANTS
// ============================================================================

#define BUTTON_DEBOUNCE_MS 50          // Button debounce time
#define DISPLAY_UPDATE_INTERVAL 1000   // Update display every second
#define STATUS_UPDATE_INTERVAL 60000   // Update status every minute
#define SCHEDULE_CHECK_INTERVAL 30000  // Check schedule every 30 seconds

// ============================================================================
// DEBUG SETTINGS
// ============================================================================

#define ENABLE_SERIAL_DEBUG true
#define SERIAL_BAUD_RATE 115200

// Debug macros
#if ENABLE_SERIAL_DEBUG
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
    #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(...)
#endif

// ============================================================================
// NODE IDENTITY
// ============================================================================

#define DEFAULT_NODE_ID "node"
#define DEFAULT_ROLE "master"  // overridden by config.json

// ============================================================================
// STRUCTURES
// ============================================================================

// Feature flags — loaded from config.json at runtime
struct Features {
    bool multi_node;
    bool mqtt;
    bool web_ui;
    bool sensors;
    bool battery;
    bool ota;
    bool debug;
};

// Global feature flags (defined in main.cpp)
extern Features features;
extern String nodeId;
extern String nodeRole;

// Irrigation schedule structure
struct IrrigationSchedule {
    bool enabled;
    uint8_t channel;           // Channel number (1-4)
    uint8_t hour;              // 0-23
    uint8_t minute;            // 0-59
    uint16_t durationMinutes;  // Duration in minutes
    uint8_t weekdays;          // Bitmask: bit 0=Sunday, bit 1=Monday, etc.
};

// System status structure
struct SystemStatus {
    bool wifiConnected;
    bool mqttConnected;
    bool irrigating;           // True if any channel is irrigating
    bool channelIrrigating[MAX_CHANNELS];  // Per-channel irrigation status
    bool channelInverted[MAX_CHANNELS];    // Per-channel invert setting (for active-low relays)
    bool manualMode;
    unsigned long irrigationStartTime;
    unsigned long channelStartTime[MAX_CHANNELS];  // Per-channel start times
    time_t lastIrrigationTime;
    time_t nextScheduledTime;
    uint16_t currentDuration;
    uint16_t channelDuration[MAX_CHANNELS];  // Per-channel durations
    String lastError;
};

// Channel pin mapping array — only local (GPIO-driven) channels
#ifdef BOARD_B
const uint8_t CHANNEL_PINS[NUM_LOCAL_CHANNELS] = {
    CHANNEL_1_PIN
};
#else
const uint8_t CHANNEL_PINS[NUM_LOCAL_CHANNELS] = {
    CHANNEL_1_PIN, CHANNEL_2_PIN, CHANNEL_3_PIN,
    CHANNEL_4_PIN, CHANNEL_5_PIN, CHANNEL_6_PIN
};
#endif

// ============================================================================
// NODE NETWORKING (UDP + mDNS)
// ============================================================================

#define NODE_UDP_PORT             4210    // UDP port for node communication
#define NODE_HEARTBEAT_INTERVAL   30000   // Send heartbeat every 30s
#define NODE_STATUS_INTERVAL      10000   // Slave sends status every 10s
#define NODE_PEER_TIMEOUT         90000   // Mark peer offline after 90s silence
#define NODE_MAX_RETRIES          3       // Retry count for ACK-requiring commands
#define NODE_DEDUP_WINDOW         60000   // Dedup seq numbers for 60s
#define NODE_MDNS_RETRY_INTERVAL  30000   // Retry mDNS discovery every 30s
#define NODE_PAIR_RETRY_INTERVAL  30000   // Slave retries PAIR_REQUEST every 30s
#define NODE_PAIR_REQUEST_TIMEOUT 60000   // Master auto-rejects pending pair after 60s

#endif // CONFIG_H
