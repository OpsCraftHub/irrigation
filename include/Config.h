#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// FIRMWARE VERSION
// ============================================================================
#ifndef VERSION
#define VERSION "1.0.0"
#endif

// ============================================================================
// PIN DEFINITIONS
// ============================================================================

// Relay/MOSFET Output for Valve Control
#define VALVE_PIN 25  // GPIO25 - Can handle relay/MOSFET

// Button Inputs (active LOW with internal pullup)
#define BTN_START 32   // Start irrigation manually
#define BTN_STOP 33    // Stop irrigation
#define BTN_NEXT 26    // Navigate menu (next item)
#define BTN_SELECT 27  // Select/confirm in menu

// LCD I2C Configuration
#define LCD_ADDRESS 0x27  // Common I2C address for LCD
#define LCD_COLS 20       // 20x4 LCD
#define LCD_ROWS 4
#define LCD_SDA 21        // I2C SDA pin
#define LCD_SCL 22        // I2C SCL pin

// Status LEDs
#define LED_STATUS 2      // Built-in LED on most ESP32 boards (Red - Power)
#define LED_BLUE 15       // Blue LED - Irrigation indicator

// ============================================================================
// IRRIGATION SETTINGS
// ============================================================================

#define MAX_SCHEDULES 4              // Maximum number of irrigation schedules
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
#define GITHUB_FIRMWARE_PATH "firmware/firmware.bin"
#define GITHUB_VERSION_PATH "firmware/version.txt"

// ============================================================================
// STORAGE SETTINGS
// ============================================================================

#define CONFIG_FILE "/config.json"
#define SCHEDULE_FILE "/schedule.json"
#define LOG_FILE "/irrigation.log"
#define MAX_LOG_ENTRIES 100

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
// STRUCTURES
// ============================================================================

// Irrigation schedule structure
struct IrrigationSchedule {
    bool enabled;
    uint8_t hour;              // 0-23
    uint8_t minute;            // 0-59
    uint16_t durationMinutes;  // Duration in minutes
    uint8_t weekdays;          // Bitmask: bit 0=Sunday, bit 1=Monday, etc.
};

// System status structure
struct SystemStatus {
    bool wifiConnected;
    bool mqttConnected;
    bool irrigating;
    bool manualMode;
    unsigned long irrigationStartTime;
    time_t lastIrrigationTime;
    time_t nextScheduledTime;
    uint16_t currentDuration;
    String lastError;
};

#endif // CONFIG_H
