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
# Build Board A firmware (ESP32 master)
pio run --environment esp32dev

# Build Board B firmware (ESP32-C3 slave) — when env exists
pio run --environment board_b_c3

# Clean build (required after version changes)
pio run --target clean --environment esp32dev

# Upload firmware to ESP32 (Board A)
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

## Hardware Design (KiCad)

### Two Boards

| Board | MCU | Purpose | Status |
|-------|-----|---------|--------|
| **Board A** | ESP32-WROOM-32U | Multi-channel master (2/4/6ch), LCD, buttons | Designed, populating |
| **Board B** | ESP32-C3-MINI-1 | Single-channel slave, optional battery | New design needed |

### Board A — Project Location

KiCad project: `hardware/irrigation_6ch_oled/irrigation_6ch_oled.kicad_pro`

### Board A — PCB Specifications

- **Board**: 110mm x 125mm, 2-layer, FR4, 1.6mm, 1oz Cu, HASL
- **Manufacturer**: JLCPCB or PCBWay (professional 2-layer fab)
- **Min trace/space**: 0.25mm / 0.25mm
- **Min drill**: 0.2mm (ESP32 thermal vias)

### Board A — Key Design Decisions

1. **ESP32-WROOM-32U (not -32D)**: External antenna variant for better WiFi in enclosures
2. **IRLZ44N logic-level MOSFETs**: Direct 3.3V gate drive from ESP32, no level shifter needed
3. **Through-hole components**: Easier hand assembly for small production runs
4. **24V solenoid drive**: MOSFET low-side switching with 1N4007 flyback protection per channel
5. **On-board 3.3V LDO (LD1117V33)**: Fed from 5V buck converter
6. **On-board LM2596T-5 buck converter**: TO-220-5 through-hole, fixed 5V output, with 1N5822 Schottky catch diode and 33uH radial inductor. Replaces the previous external buck module header (J4).
7. **6-pin UART header (J10)**: Includes TX, RX, +3V3, GND for programming. **TODO: Add GPIO0 and EN pins for one-click programming**

### Board B — Key Design Decisions

1. **ESP32-C3-MINI-1**: RISC-V, lower cost, lower power (5uA deep sleep), no GPIO0/2 boot-strap issues
2. **SMD module + through-hole passives**: C3-MINI-1 is castellated SMD; all other parts stay through-hole
3. **Single MOSFET channel**: Same IRLZ44N pattern as Board A
4. **Jumper J1**: Selects mains power or battery power mode
5. **Status LED on GPIO3**: Morse-style patterns for headless debugging

### MOSFET Channel Pattern (x6)

Each valve channel follows the same pattern:
```
ESP32 GPIO --> 100R gate resistor --> IRLZ44N gate
                                      IRLZ44N drain --> solenoid --> +24V
                                      IRLZ44N source --> GND
                  10k pull-down --> IRLZ44N gate to GND
                  1N4007 flyback diode across drain-source
```

### Autorouting Workflow

Uses Freerouting (external Java autorouter):
```bash
# 1. Export DSN from KiCad Python API
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -c "
import pcbnew
board = pcbnew.LoadBoard('hardware/irrigation_6ch_oled/irrigation_6ch_oled.kicad_pcb')
pcbnew.ExportSpecctraDSN(board, 'hardware/irrigation_6ch_oled/irrigation_6ch_oled.dsn')
"

# 2. Run Freerouting
java -jar ~/freerouting-2.0.1.jar -de hardware/irrigation_6ch_oled/irrigation_6ch_oled.dsn -do hardware/irrigation_6ch_oled/irrigation_6ch_oled.ses -mp 30

# 3. Import SES back
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -c "
import pcbnew
board = pcbnew.LoadBoard('hardware/irrigation_6ch_oled/irrigation_6ch_oled.kicad_pcb')
pcbnew.ImportSpecctraSES(board, 'hardware/irrigation_6ch_oled/irrigation_6ch_oled.ses')
board.Save(board.GetFileName())
"

# 4. Run DRC
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli pcb drc --output /tmp/drc_report.json --format json "hardware/irrigation_6ch_oled/irrigation_6ch_oled.kicad_pcb"
```

### KiCad Python API Notes

- System Python does NOT have pcbnew. Must use KiCad's bundled Python:
  `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3`
- `kicad-cli` does NOT support DSN export - must use pcbnew Python API
- Positions in pcbnew use nanometers internally: `pcbnew.FromMM(x)` and `pcbnew.ToMM(x)`
- Footprint library path: `/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/`

## Product Line Strategy

### Context

This is the first of ~10 home electronics products built on ESP32 + Home Assistant. Optimizing for **fast iteration and R&D time to market**.

### Reusable Design Patterns Across Products

1. **ESP32 + Home Assistant core**: Master boards use ESP32-WROOM-32U, slave/satellite boards use ESP32-C3-MINI-1. WiFi, MQTT auto-discovery, OTA updates
2. **Firmware architecture**: 4-component pattern (Controller, DisplayManager, WiFiManager, HomeAssistantIntegration) - copy and adapt per product
3. **Power supply**: 24V/12V input → buck → 5V → LD1117V33 → 3.3V (standardize across products)
4. **MOSFET switching pattern**: IRLZ44N + 100R gate + 10k pulldown + flyback diode - reuse for any solenoid/relay/motor load
5. **PCB manufacturing**: JLCPCB for production boards, through-hole for hand assembly at small scale
6. **OTA updates**: GitHub Actions auto-build + version bump + device auto-update from GitHub releases
7. **Enclosure**: M3 mounting holes at corners, standard spacing

### Fast Iteration Checklist (New Product)

1. Copy firmware template (4-component architecture)
2. Copy KiCad schematic template (ESP32 + power + MOSFET channels)
3. Modify channel count and peripherals as needed
4. Adapt Home Assistant discovery messages
5. Route PCB with Freerouting
6. Order from JLCPCB ($2-5 for 5 boards, 5-7 day turnaround)
7. Hand-assemble and test
8. Set up GitHub Actions for OTA pipeline

### BOM Cost Targets

- **Per unit (1-off)**: ~$8-10 USD (~R150-180 ZAR)
- **Per unit (50-pack bulk)**: ~$5.50-7 USD (~R100-130 ZAR)
- **PCB only**: ~$1/board at 5-pack, ~$0.40/board at 50-pack (JLCPCB)
- **Target retail margin**: 3-4x BOM cost

### Suppliers

- **PCB fabrication**: JLCPCB (cheapest), PCBWay (better service)
- **Components**: AliExpress (bulk, 2-4 week shipping to Cape Town)
- **Local SA backup**: PiShop.co.za, Leobot.net, Micro Robotics (robotics.org.za)
- **Enclosures**: AliExpress project boxes or custom 3D-printed

## Documentation

- `README.md` - Complete user guide, hardware wiring, setup
- `TECHNICAL.md` - Architecture diagrams, state machines, performance metrics
- `PROJECT_SUMMARY.md` - Feature matrix, code statistics, project overview
- `QUICKSTART.md` - 30-minute setup guide
