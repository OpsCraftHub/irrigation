# ESP32 Single-Channel Irrigation Controller

A comprehensive irrigation controller built on ESP32 with Home Assistant integration, OTA updates, and offline scheduling capabilities.

## Features

### 💧 Core Functionality
- **Single-channel control** - Control one irrigation valve or pump via relay/MOSFET
- **Schedule-based irrigation** - Up to 4 independent schedules with day-of-week selection
- **Offline operation** - Continues running schedules even without internet
- **Safety features** - Automatic timeout and manual emergency stop
- **Non-volatile storage** - Schedules persist through power cycles

### 🖥️ User Interface
- **20x4 LCD Display** - Clear status and menu navigation
- **Physical buttons** - Start, Stop, Next, and Select for easy control
- **Real-time feedback** - Shows current status, time remaining, and next scheduled run
- **Menu system** - Easy configuration without computer access

### 🌐 Connectivity
- **WiFi with auto-reconnect** - Robust connection management
- **NTP time synchronization** - Accurate scheduling with automatic DST
- **OTA firmware updates** - Update from GitHub repository automatically
- **MQTT integration** - Full Home Assistant discovery and control

### 🏠 Home Assistant Integration
- **Auto-discovery** - Automatic entity creation via MQTT
- **Remote control** - Start/stop irrigation from anywhere
- **Status monitoring** - Real-time updates on irrigation status
- **Customizable automations** - Integrate with weather, schedules, and more

## Hardware Requirements

### Required Components
- **ESP32 Development Board** (DevKit, NodeMCU, etc.)
- **5V Relay Module** or **Logic Level MOSFET** (for valve control)
- **20x4 I2C LCD Display** (or 16x2, adjust Config.h)
- **4x Push Buttons** (Start, Stop, Next, Select)
- **12V Solenoid Valve** or **Water Pump**
- **12V Power Supply** (for valve/pump)
- **5V Power Supply** (for ESP32) or use voltage regulator

### Optional Components
- **DS3231 RTC Module** (for time keeping when offline)
- **Rain Sensor** (connect via Home Assistant)
- **Soil Moisture Sensor** (connect via Home Assistant)

## Pin Configuration

Default pin assignments in `include/Config.h`:

```cpp
// Relay/MOSFET Output
#define VALVE_PIN 25

// Button Inputs (with internal pullup)
#define BTN_START 32
#define BTN_STOP 33
#define BTN_NEXT 26
#define BTN_SELECT 27

// LCD I2C
#define LCD_SDA 21
#define LCD_SCL 22
#define LCD_ADDRESS 0x27

// Status LED
#define LED_STATUS 2
```

## Wiring Diagram

```
ESP32          Relay Module        Valve
GPIO25 ------> IN                  +12V --- Valve --- COM
GND ----------> GND                NC (not used)
              VCC <----- 5V        NO ---------------+

ESP32          LCD (I2C)
GPIO21 ------> SDA
GPIO22 ------> SCL
5V -----------> VCC
GND ----------> GND

ESP32          Buttons (Active LOW)
GPIO32 ------> Start Button --> GND
GPIO33 ------> Stop Button ---> GND
GPIO26 ------> Next Button ---> GND
GPIO27 ------> Select Button -> GND
```

## Software Setup

### 1. Install PlatformIO

```bash
# Install PlatformIO Core
pip install platformio

# Or use PlatformIO IDE extension for VSCode
```

### 2. Clone and Configure

```bash
git clone https://github.com/yourusername/irrigation-controller.git
cd irrigation-controller

# Edit configuration
nano include/Config.h
```

Update these settings in `Config.h`:
- WiFi credentials
- MQTT broker IP
- Timezone offset
- GitHub repository details

### 3. Upload Filesystem

```bash
# Upload SPIFFS filesystem (includes config.json)
pio run --target uploadfs
```

### 4. Compile and Upload

```bash
# Build and upload firmware
pio run --target upload

# Monitor serial output
pio device monitor
```

## Configuration

### WiFi and MQTT

Edit `include/Config.h` or create `data/config.json`:

```json
{
  "wifi": {
    "ssid": "YourNetwork",
    "password": "YourPassword"
  },
  "mqtt": {
    "broker": "192.168.1.100",
    "port": 1883,
    "user": "mqtt_user",
    "password": "mqtt_password"
  }
}
```

### Schedules

Schedules can be configured via:
1. **LCD Menu** - Use buttons to navigate and set schedules
2. **Code** - Edit `src/main.cpp` setup() function
3. **Home Assistant** - Through MQTT commands (future enhancement)

Schedule format:
```cpp
irrigationController->addSchedule(
    0,      // Schedule index (0-3)
    6,      // Hour (0-23)
    0,      // Minute (0-59)
    30,     // Duration in minutes
    0x7F    // Weekdays bitmask (0x7F = all days)
);
```

Weekday bitmask:
- `0x7F` = All days (1111111)
- `0x3E` = Weekdays only (0111110)
- `0x41` = Weekend only (1000001)
- Bit 0 = Sunday, Bit 1 = Monday, etc.

## Usage

### Button Controls

- **START** - Begin irrigation immediately (uses default duration)
- **STOP** - Stop current irrigation and return to status screen
- **NEXT** - Navigate through menu items / Enter menu from status
- **SELECT** - Select menu item / Return to menu

### LCD Display

**Status Screen** (default):
```
WiFi:OK MQTT:OK
IDLE
Last: 11/12 06:00
Next: 11/12 18:00
```

**Menu Navigation**:
- View current status
- Manual control
- View/edit schedules
- System information

## Home Assistant Integration

### Automatic Discovery

The device will automatically appear in Home Assistant after:
1. MQTT is configured correctly
2. MQTT discovery is enabled in HA
3. Device connects to network

### Entities Created

- `switch.irrigation_controller` - Main control switch
- `sensor.irrigation_controller_status` - Detailed status with attributes
- `number.irrigation_controller_duration` - Duration control

### Example Lovelace Card

```yaml
type: vertical-stack
cards:
  - type: entities
    title: Irrigation
    entities:
      - entity: switch.irrigation_controller
      - entity: sensor.irrigation_time_remaining
      - entity: sensor.next_irrigation
```

See `home-assistant/irrigation.yaml` for complete examples including:
- Automations
- Scripts
- Template sensors
- Dashboard cards

## OTA Updates

### Automatic Updates from GitHub

The controller checks GitHub daily for new firmware:

1. **Setup GitHub Repository**:
   - Create `firmware/firmware.bin` (compiled binary)
   - Create `firmware/version.txt` (version string)

2. **Configure** in `include/Config.h`:
   ```cpp
   #define GITHUB_REPO_OWNER "yourusername"
   #define GITHUB_REPO_NAME "irrigation-controller"
   ```

3. **Build Firmware**:
   ```bash
   pio run
   cp .pio/build/esp32dev/firmware.bin firmware/
   echo "1.0.1" > firmware/version.txt
   git add firmware/
   git commit -m "Release v1.0.1"
   git push
   ```

### Manual OTA via Arduino OTA

```bash
# Update OTA_PASSWORD in Config.h first
pio run --target upload --upload-port irrigation-esp32.local
```

## System Architecture

### Main Loop Flow

```
setup()
├── Initialize Serial
├── Load Configuration
├── Initialize IrrigationController
│   ├── Load schedules from SPIFFS
│   └── Configure valve pin
├── Initialize DisplayManager
│   ├── Setup LCD
│   └── Configure buttons
├── Initialize WiFiManager
│   ├── Connect to WiFi
│   ├── Setup OTA
│   └── Start NTP sync
└── Initialize HomeAssistant
    ├── Connect to MQTT
    └── Publish discovery

loop()
├── irrigationController.update()
│   ├── Check schedules
│   ├── Update irrigation state
│   └── Safety timeout check
├── displayManager.update()
│   ├── Update LCD display
│   └── Check button presses
├── wifiManager.update()
│   ├── Handle WiFi reconnection
│   ├── Sync NTP time
│   ├── Handle OTA requests
│   └── Check for firmware updates
└── homeAssistant.update()
    ├── Maintain MQTT connection
    ├── Process commands
    └── Publish status updates
```

### Class Architecture

```
IrrigationController
├── Schedule management
├── Valve control
├── Time-based execution
└── SPIFFS storage

DisplayManager
├── LCD rendering
├── Button handling
└── Menu navigation

WiFiManager
├── WiFi connection
├── NTP synchronization
├── OTA updates
└── GitHub firmware checking

HomeAssistantIntegration
├── MQTT communication
├── Auto-discovery
├── Command handling
└── Status publishing
```

## Troubleshooting

### LCD Not Working
- Check I2C address (0x27 or 0x3F common)
- Verify SDA/SCL connections
- Run I2C scanner to detect address

### WiFi Connection Issues
- Verify credentials in Config.h
- Check signal strength
- Try static IP configuration
- Monitor serial output for errors

### MQTT Not Connecting
- Verify broker IP and port
- Check username/password
- Ensure MQTT broker is running
- Check firewall rules

### Schedules Not Running
- Verify time is synchronized (check LCD)
- Confirm schedules are enabled
- Check weekday bitmask
- Monitor serial debug output

### OTA Update Fails
- Check GitHub URL and credentials
- Verify firmware.bin size (<1.4MB)
- Ensure stable WiFi connection
- Check available flash space

## Safety Features

1. **Safety Timeout** - Automatically stops irrigation after maximum duration (default: 5 hours)
2. **Manual Override** - Physical stop button works at all times
3. **Watchdog Protection** - Prevents system hangs
4. **Error Recovery** - Graceful degradation when components fail
5. **Offline Operation** - Works without internet/MQTT

## Future Enhancements

- [ ] Multiple channel support
- [ ] Flow meter integration
- [ ] Weather API integration
- [ ] Web-based configuration
- [ ] Historical logging
- [ ] Mobile app
- [ ] Voice control (Alexa/Google)

## License

MIT License - Feel free to modify and use for your own projects!

## Contributing

Contributions welcome! Please submit pull requests or open issues.

## Author

Created by [Your Name]

## Acknowledgments

- ESP32 Arduino Core
- PlatformIO
- Home Assistant Community
- ArduinoJson library
- LiquidCrystal_I2C library
