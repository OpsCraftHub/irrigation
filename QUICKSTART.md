# Quick Start Guide
## ESP32 Irrigation Controller

Get your irrigation controller up and running in under 30 minutes!

## Prerequisites

- [ ] ESP32 development board
- [ ] USB cable
- [ ] Computer with PlatformIO installed
- [ ] 5V Relay module
- [ ] 20x4 I2C LCD display
- [ ] 4 push buttons
- [ ] Jumper wires

## Step 1: Hardware Assembly (10 minutes)

### Minimal Wiring for Testing

```
ESP32 GPIO25 ──→ Relay IN
ESP32 GND ─────→ Relay GND
ESP32 5V ──────→ Relay VCC

ESP32 GPIO21 ──→ LCD SDA
ESP32 GPIO22 ──→ LCD SCL
ESP32 GND ─────→ LCD GND
ESP32 5V ──────→ LCD VCC

ESP32 GPIO32 ──→ Button (START) ──→ GND
ESP32 GPIO33 ──→ Button (STOP) ───→ GND
ESP32 GPIO26 ──→ Button (NEXT) ───→ GND
ESP32 GPIO27 ──→ Button (SELECT) ─→ GND
```

**Note**: ESP32 has internal pull-up resistors, so no external resistors needed for buttons!

## Step 2: Software Setup (5 minutes)

### Install PlatformIO

**Option A: VSCode Extension**
1. Install VSCode
2. Open Extensions (Ctrl+Shift+X)
3. Search "PlatformIO IDE"
4. Click Install

**Option B: Command Line**
```bash
pip install platformio
```

### Clone or Download Project

```bash
# If you have the code
cd ~/Documents/irigation

# Or clone from GitHub
git clone <your-repo-url> irrigation-controller
cd irrigation-controller
```

## Step 3: Configure Settings (5 minutes)

### Edit WiFi and MQTT Settings

Open `include/Config.h` and modify:

```cpp
// Line 53: WiFi Settings
#define WIFI_SSID "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"

// Line 62: MQTT Settings (Home Assistant IP)
#define MQTT_BROKER "192.168.1.100"  // Your HA IP address
#define MQTT_USER "mqtt_user"        // Your MQTT username
#define MQTT_PASSWORD "mqtt_password" // Your MQTT password
```

### Verify Pin Assignments (if needed)

Check pins match your wiring in `include/Config.h` (lines 17-33).

## Step 4: Upload to ESP32 (5 minutes)

### Connect ESP32 to Computer

1. Plug in USB cable
2. Check that ESP32 is detected

### Upload Firmware

**Using VSCode:**
1. Open project folder
2. Click PlatformIO icon (alien head)
3. Click "Upload" under esp32dev

**Using Command Line:**
```bash
pio run --target upload
```

### Upload Configuration Files

```bash
pio run --target uploadfs
```

### Monitor Serial Output (Optional but Recommended)

**VSCode**: Click "Monitor" in PlatformIO

**Command Line**:
```bash
pio device monitor
```

You should see:
```
==================================
ESP32 Irrigation Controller
Version: 1.0.0
==================================

WiFiManager: Connecting to YourWiFiName
WiFiManager: Connected! IP: 192.168.1.XXX
...
System initialized successfully!
```

## Step 5: First Test (5 minutes)

### Test LCD Display

The LCD should show:
```
WiFi:OK MQTT:OK
IDLE
Last: N/A
Next: 11/12 06:00
```

### Test Manual Control

1. Press **START** button
   - Relay should click ON
   - LCD shows "IRRIGATING (MAN)"
   - LED on ESP32 blinks

2. Press **STOP** button
   - Relay clicks OFF
   - LCD shows "IDLE"

### Test Menu Navigation

1. Press **NEXT** button → Enter menu
2. Press **NEXT** again → Navigate menu items
3. Press **SELECT** → Select item or go back

## Step 6: Home Assistant Setup (5 minutes)

### Check Auto-Discovery

1. Open Home Assistant
2. Go to **Settings** → **Devices & Services**
3. Look for **MQTT** integration
4. Click on MQTT
5. Find **"Irrigation Controller"** device

If not visible, check:
- MQTT broker is running
- ESP32 is connected (check serial monitor)
- Discovery is enabled in HA MQTT config

### Add to Dashboard

Quick add:
1. Go to your dashboard
2. Click **Edit Dashboard**
3. Click **+ Add Card**
4. Search for "Irrigation Controller"
5. Select entities you want

Or use the example card from `home-assistant/irrigation.yaml`

## Step 7: Add Schedules (5 minutes)

### Option A: Via Main.cpp (Recommended for First Setup)

Edit `src/main.cpp` around line 82:

```cpp
// Schedule 1: Every day at 6:00 AM for 30 minutes
irrigationController->addSchedule(0, 6, 0, 30, 0x7F);

// Schedule 2: Every day at 6:00 PM for 20 minutes
irrigationController->addSchedule(1, 18, 0, 20, 0x7F);

// Schedule 3: Weekdays only at 8:00 AM for 15 minutes
// 0x3E = Mon-Fri (0111110 in binary)
irrigationController->addSchedule(2, 8, 0, 15, 0x3E);
```

Then re-upload firmware.

### Option B: Via LCD Menu (Not fully implemented in v1.0)

Future feature - will allow button-based schedule editing.

### Weekday Bitmask Reference

```
Bit:   6 5 4 3 2 1 0
Day:   S F T W T M S
       a r h u h o u
       t i u e e n n

All days: 0x7F = 1111111
Weekdays: 0x3E = 0111110
Weekend:  0x41 = 1000001
Mon/Wed:  0x0A = 0001010
```

## Troubleshooting Quick Fixes

### LCD Not Working

```bash
# Test I2C scanner - add to setup():
Wire.begin(21, 22);
for (uint8_t addr = 0x20; addr < 0x28; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
        Serial.printf("LCD found at 0x%02X\n", addr);
    }
}
```

Common addresses: `0x27` or `0x3F`

Update `Config.h` line 29 if needed.

### WiFi Not Connecting

1. Check SSID/password in `Config.h`
2. Make sure ESP32 is within WiFi range
3. Check 2.4GHz WiFi (ESP32 doesn't support 5GHz)
4. Monitor serial output for error messages

### MQTT Not Connecting

1. Verify MQTT broker is running:
   ```bash
   # On Home Assistant
   ha addons info core_mosquitto
   ```

2. Test MQTT from computer:
   ```bash
   mosquitto_sub -h 192.168.1.100 -t "#" -u mqtt_user -P mqtt_password
   ```

3. Check firewall isn't blocking port 1883

### Relay Not Switching

1. Check wiring (especially GND connection)
2. Verify GPIO25 is correct pin (check board pinout)
3. Test relay manually:
   ```cpp
   // Add to loop() for testing
   digitalWrite(25, HIGH);
   delay(2000);
   digitalWrite(25, LOW);
   delay(2000);
   ```

### Schedules Not Running

1. Check time is synced:
   - LCD should show correct time
   - Serial monitor: `Time synced: <timestamp>`

2. Verify schedule is enabled and correct time

3. Check you're in correct timezone (Config.h line 75)

## What's Next?

### Connect the Valve

⚠️ **WARNING**: Working with 12V/24V and water. Take precautions!

```
12V Power Supply (+) ──→ Valve (+)
Valve (-) ─────────────→ Relay COM
Relay NO ──────────────→ 12V Power Supply (-)
```

**Safety Tips**:
- Use waterproof enclosure for electronics
- Keep 12V circuit separate from 5V ESP32
- Test relay with LED first before connecting valve
- Add flyback diode across solenoid coil
- Use appropriate wire gauge for valve current

### Customize

1. **Adjust Schedules**: Edit `main.cpp` or use future menu system
2. **Change Timings**: Modify intervals in `Config.h`
3. **Add Automations**: Use Home Assistant YAML examples
4. **Setup OTA**: Configure GitHub repo for remote updates

### Advanced Features

- [ ] Add rain sensor integration (via HA automation)
- [ ] Setup weather-based skip logic
- [ ] Create custom Lovelace dashboard
- [ ] Enable data logging and charts
- [ ] Setup notifications
- [ ] Add flow meter for water usage tracking

## Support

### Useful Serial Commands for Debugging

Add to `loop()` in main.cpp:

```cpp
if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    if (cmd == "status") {
        // Print system status
    } else if (cmd == "start") {
        irrigationController->startIrrigation(5);
    } else if (cmd == "stop") {
        irrigationController->stopIrrigation();
    } else if (cmd == "time") {
        Serial.println(irrigationController->getCurrentTime());
    }
}
```

### Getting Help

1. Check `README.md` for detailed documentation
2. Review `TECHNICAL.md` for architecture details
3. Monitor serial output for error messages
4. Check Home Assistant logs: Settings → System → Logs
5. Post issues on GitHub (if applicable)

## Success Checklist

After completing this guide, you should have:

- [x] ESP32 connected and powered
- [x] LCD displaying status
- [x] Buttons responding
- [x] Relay switching
- [x] WiFi connected
- [x] Time synchronized
- [x] MQTT connected to Home Assistant
- [x] Device auto-discovered in HA
- [x] Manual control working
- [x] Schedules configured
- [x] Ready to connect valve!

**Congratulations!** 🎉 Your irrigation controller is ready!

---

**Total Setup Time**: ~30 minutes
**Difficulty**: Beginner to Intermediate
**Next Steps**: See README.md for full documentation
