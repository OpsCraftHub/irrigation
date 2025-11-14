# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an **ESP32-based irrigation controller** with Home Assistant integration, featuring:
- Single-channel valve control via relay/MOSFET
- Offline schedule-based irrigation (up to 4 schedules)
- 20x4 LCD display with 4-button interface
- WiFi connectivity with auto-reconnect
- MQTT/Home Assistant integration with auto-discovery
- Automatic OTA firmware updates from GitHub
- Non-volatile schedule storage (SPIFFS)

## Build & Development Commands

### PlatformIO Commands

```bash
# Build firmware
pio run --environment esp32dev

# Clean build (required after version changes)
pio run --target clean --environment esp32dev

# Upload firmware to ESP32
pio run --target upload --upload-port /dev/cu.usbserial-0001

# Upload SPIFFS filesystem (config.json)
pio run --target uploadfs

# Serial monitor
pio device monitor --port /dev/cu.usbserial-0001 --baud 115200

# List connected devices
pio device list
```

### Python Serial Monitor

Use `monitor_serial.py` for quick 10-second output capture:
```bash
python3 monitor_serial.py
```

### Git Workflow

This project uses automated version management:
```bash
# Any push to main triggers automatic version bump
git add <files>
git commit -m "Your change description"
git push origin main
```

The GitHub Actions workflow will automatically:
1. Bump the patch version in `include/Config.h`
2. Clean and rebuild firmware with new version
3. Commit `Config.h`, `firmware.bin`, and `version.txt` atomically
4. Create GitHub release with tag

## Architecture Overview

### Four Main Components

**IrrigationController** (`src/IrrigationController.cpp`)
- Core irrigation logic and state machine
- Schedule management (up to 4 schedules)
- Valve control with safety timeout (5-hour max)
- SPIFFS-based persistent schedule storage
- Time-based execution engine

**DisplayManager** (`src/DisplayManager.cpp`)
- 20x4 LCD I2C display rendering
- 4-button input handling with debounce (START, STOP, NEXT, SELECT)
- Menu navigation system
- Status screen rendering
- All user interface logic

**WiFiManager** (`src/WiFiManager.cpp`)
- WiFi connection with auto-reconnect (every 30s)
- NTP time synchronization (hourly)
- Arduino OTA support
- GitHub firmware version checking (daily)
- Automatic firmware download and OTA update

**HomeAssistantIntegration** (`src/HomeAssistantIntegration.cpp`)
- MQTT broker connection with auto-reconnect (every 5s)
- Home Assistant MQTT discovery protocol
- Command handling (start/stop via MQTT)
- Status publishing (every 60s)
- Auto-discovered entities: switch, sensor, number

### Component Interaction Flow

```
main.cpp setup()
  ├─> IrrigationController.begin()  // Load schedules from SPIFFS
  ├─> DisplayManager.begin()        // Init LCD, configure buttons
  ├─> WiFiManager.begin()           // Connect WiFi, sync NTP, setup OTA
  └─> HomeAssistant.begin()         // Connect MQTT, publish discovery

main.cpp loop()
  ├─> IrrigationController.update() // Check schedules, update state
  ├─> DisplayManager.update()       // Render LCD, check buttons
  ├─> WiFiManager.update()          // Handle WiFi, OTA, time sync
  └─> HomeAssistant.update()        // MQTT connection, commands, status
```

### State Machine

The IrrigationController operates as a state machine:
- **IDLE**: Waiting for schedule or manual trigger
- **IRRIGATING**: Valve open, timer running
- **STOPPING**: Valve closing, saving state

Transitions occur via:
- Schedule time match (checked every 30s)
- Manual button press (START/STOP)
- MQTT command from Home Assistant
- Safety timeout (5 hours)

## Critical Implementation Details

### Version Management System

**CRITICAL**: VERSION must ONLY be defined in `include/Config.h` as the single source of truth.

```cpp
// include/Config.h
#define VERSION "1.0.5"
```

**DO NOT** add VERSION to `platformio.ini` build flags. The automated workflow:
1. Reads VERSION from Config.h
2. Increments patch number
3. Updates Config.h
4. Runs clean build to ensure version is compiled in
5. Commits Config.h + firmware.bin + version.txt atomically

This prevents infinite OTA update loops where the device downloads firmware but restarts with the old version.

### GitHub Actions Workflow

Located at `.github/workflows/build-firmware.yml`:
- Triggers on push to main when `src/`, `include/`, or `platformio.ini` changes
- Automatically bumps patch version
- Clean build ensures version string is updated in binary
- Atomic commit of Config.h, firmware.bin, and version.txt prevents version mismatch
- Creates GitHub release with OTA-ready firmware

### OTA Update Flow

1. Device checks GitHub daily: `https://raw.githubusercontent.com/[repo]/main/firmware/version.txt`
2. Compares with VERSION constant compiled into firmware
3. If newer version available, downloads `firmware/firmware.bin`
4. Performs OTA update and restarts
5. On boot, displays new version on LCD
6. Checks for updates, finds it's current, stops (no loop)

### SPIFFS Storage

Schedules are stored in JSON format at `/schedules.json`:
```json
{
  "schedules": [
    {"enabled": true, "hour": 6, "minute": 0, "duration": 30, "weekdays": 127},
    {"enabled": true, "hour": 18, "minute": 0, "duration": 20, "weekdays": 127}
  ]
}
```

### Weekday Bitmask

Schedules use a bitmask for day-of-week selection:
```
Bit 0 = Sunday, Bit 1 = Monday, ..., Bit 6 = Saturday
0x7F (127) = All days (1111111)
0x3E (62)  = Weekdays only (0111110)
0x41 (65)  = Weekend only (1000001)
```

### Button Debouncing

All buttons use 50ms debounce in `DisplayManager::debounceButton()`. Buttons are active LOW with internal pullup resistors.

### Safety Features

- **Safety Timeout**: Irrigation automatically stops after 5 hours (SAFETY_TIMEOUT_MINUTES)
- **Manual Override**: Physical STOP button works at all times, bypassing all states
- **Graceful Degradation**: System continues operating if LCD, MQTT, or WiFi fails
- **Offline Operation**: Schedules run without internet (requires initial NTP sync)

## Home Assistant Integration

### MQTT Topics

Discovery and control topics follow Home Assistant convention:
```
homeassistant/switch/irrigation_esp32_[MAC]/config    (discovery)
homeassistant/switch/irrigation_esp32_[MAC]/command   (control)
homeassistant/switch/irrigation_esp32_[MAC]/state     (state)
homeassistant/sensor/irrigation_esp32_[MAC]/status    (attributes)
```

### Auto-Discovery

On MQTT connection, the device publishes discovery messages creating:
- `switch.irrigation_controller` - Main on/off control
- `sensor.irrigation_controller_status` - Status with attributes (time_remaining, next_scheduled, etc.)
- `number.irrigation_controller_duration` - Duration control (1-240 minutes)

## Configuration

### Pin Assignments (Config.h)

```cpp
VALVE_PIN = 25      // Relay/MOSFET output
BTN_START = 32      // Start button (active low)
BTN_STOP = 33       // Stop button (active low)
BTN_NEXT = 26       // Menu navigation
BTN_SELECT = 27     // Menu select
LCD_SDA = 21        // I2C data
LCD_SCL = 22        // I2C clock
LCD_ADDRESS = 0x27  // I2C address (may be 0x3F)
```

### WiFi/MQTT Configuration

Edit `include/Config.h` or create `data/config.json`:
```cpp
#define WIFI_SSID "YourNetwork"
#define WIFI_PASSWORD "YourPassword"
#define MQTT_BROKER "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_USER "mqtt_user"
#define MQTT_PASSWORD "mqtt_password"
```

## Common Tasks

### Adding a Schedule Programmatically

In `src/main.cpp` setup():
```cpp
irrigationController->addSchedule(
    0,      // Schedule index (0-3)
    6,      // Hour (0-23)
    0,      // Minute (0-59)
    30,     // Duration in minutes
    0x7F    // Weekdays bitmask (0x7F = all days)
);
```

### Testing OTA Updates

1. Make code changes and push to main
2. Wait for GitHub Actions to complete (~1-2 minutes)
3. Device will check for updates on next daily check or manual trigger
4. Monitor serial output to see update progress
5. Device restarts with new firmware

### Debugging Serial Output

Enable serial debug by default via `ENABLE_SERIAL_DEBUG` in Config.h:
```cpp
#define ENABLE_SERIAL_DEBUG 1
#define SERIAL_BAUD_RATE 115200
```

Use `DEBUG_PRINTLN()` and `DEBUG_PRINTF()` macros throughout code for conditional logging.

### Checking Free Memory

Monitor heap during development:
```cpp
DEBUG_PRINTF("Free heap: %d bytes\n", ESP.getFreeHeap());
```

Typical free heap during operation: ~297KB / 520KB total

## Troubleshooting

### LCD Not Showing Anything

1. Check I2C address - common addresses are 0x27 or 0x3F
2. Verify SDA/SCL pin connections
3. Run I2C scanner to detect device
4. System continues without display if init fails

### Version Not Updating After OTA

This was a previous bug. The fix:
- VERSION must ONLY be in Config.h (NOT in platformio.ini)
- Workflow includes clean build step
- All three files (Config.h, firmware.bin, version.txt) committed atomically

### Schedules Not Running

1. Check time sync: Look for "NTP client not initialized" or "Firmware is up to date" on serial
2. Verify time zone offset in Config.h
3. Check weekday bitmask matches current day
4. Ensure schedule is enabled in SPIFFS

### MQTT Not Connecting

1. Verify broker IP and port
2. Check username/password
3. Ensure MQTT broker allows connections
4. System continues without MQTT (offline mode)

## Memory Constraints

- Flash: ~600KB used of 4MB (firmware)
- RAM: ~223KB used of 520KB (43%)
- SPIFFS: 1.5MB available for storage
- Keep JSON buffers small (ArduinoJson StaticJsonDocument)
- Avoid String concatenation in loops (use String::reserve())

## Testing Recommendations

Before releasing changes:
1. Test manual start/stop via buttons
2. Verify LCD display updates
3. Test WiFi reconnect (power cycle router)
4. Test MQTT reconnect (restart broker)
5. Verify schedule triggers at correct time
6. Test OTA update process
7. Check serial output for errors
8. Monitor free heap for leaks

## Documentation

- `README.md` - Complete user guide, hardware wiring, setup
- `TECHNICAL.md` - Architecture diagrams, state machines, performance metrics
- `PROJECT_SUMMARY.md` - Feature matrix, code statistics, project overview
- `QUICKSTART.md` - 30-minute setup guide
