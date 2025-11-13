# Technical Documentation
## ESP32 Irrigation Controller

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     ESP32 Main Controller                    │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │                   Main Loop (10ms)                      │ │
│  └────────────────────────────────────────────────────────┘ │
│                              │                               │
│  ┌───────────────┬───────────┼────────────┬────────────────┐│
│  │               │            │            │                ││
│  ▼               ▼            ▼            ▼                ││
│ ┌───────┐  ┌─────────┐  ┌────────┐  ┌──────────┐          ││
│ │Irriga-│  │Display  │  │ WiFi   │  │Home      │          ││
│ │tion   │  │Manager  │  │Manager │  │Assistant │          ││
│ │Control│  └─────────┘  └────────┘  └──────────┘          ││
│ └───────┘       │            │            │                ││
│     │           │            │            │                ││
└─────┼───────────┼────────────┼────────────┼────────────────┘
      │           │            │            │
      ▼           ▼            ▼            ▼
   ┌─────┐   ┌──────┐    ┌────────┐   ┌──────┐
   │Relay│   │ LCD  │    │  WiFi  │   │ MQTT │
   │/MOSFET  │+Btns │    │ Router │   │Broker│
   └─────┘   └──────┘    └────────┘   └──────┘
      │
      ▼
   ┌──────┐
   │Valve/│
   │ Pump │
   └──────┘
```

### Component Interaction Flow

```
┌──────────────┐
│   Power On   │
└──────┬───────┘
       │
       ▼
┌──────────────────┐
│ System Init      │
│ - Serial         │
│ - SPIFFS         │
│ - GPIO           │
└──────┬───────────┘
       │
       ▼
┌──────────────────┐     ┌─────────────────┐
│ Load Config      │────▶│ Config File     │
│ - WiFi Creds     │     │ (SPIFFS)        │
│ - MQTT Settings  │     └─────────────────┘
└──────┬───────────┘
       │
       ▼
┌──────────────────┐     ┌─────────────────┐
│ Init Controllers │────▶│ Load Schedules  │
│ - Irrigation     │     │ (SPIFFS)        │
│ - Display        │     └─────────────────┘
│ - WiFi           │
│ - HomeAssistant  │
└──────┬───────────┘
       │
       ▼
┌──────────────────┐
│   Main Loop      │
│   (Continuous)   │
└──────┬───────────┘
       │
       └─────┐
             │
    ┌────────┴────────────────────────┐
    │                                 │
    ▼                                 ▼
┌─────────────┐              ┌─────────────┐
│Update       │              │Update       │
│Controllers  │              │Status       │
└─────┬───────┘              └──────┬──────┘
      │                             │
      │                             ▼
      │                      ┌──────────────┐
      │                      │Publish MQTT  │
      │                      │Update Display│
      │                      └──────────────┘
      │
      └──────────────────────────────────────┐
                                             │
                                             ▼
                                      ┌─────────────┐
                                      │Check Events │
                                      │- Buttons    │
                                      │- Schedules  │
                                      │- MQTT Cmds  │
                                      └──────┬──────┘
                                             │
                                             ▼
                                      ┌─────────────┐
                                      │Take Action  │
                                      │- Start/Stop │
                                      │- Navigate   │
                                      └─────────────┘
```

## Irrigation Controller State Machine

```
                    ┌─────────────────┐
                    │   SYSTEM START  │
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │      IDLE       │◀─────────────┐
                    │  Valve: OFF     │              │
                    │  Waiting...     │              │
                    └────────┬────────┘              │
                             │                       │
        ┌────────────────────┼────────────────┐      │
        │                    │                │      │
        ▼                    ▼                ▼      │
┌──────────────┐    ┌────────────────┐  ┌─────────┐│
│Schedule Time │    │Manual Start    │  │MQTT Cmd ││
│Reached       │    │Button Pressed  │  │Received ││
└──────┬───────┘    └────────┬───────┘  └────┬────┘│
       │                     │               │     │
       └─────────────────────┼───────────────┘     │
                             │                     │
                             ▼                     │
                    ┌─────────────────┐            │
                    │   IRRIGATING    │            │
                    │  Valve: ON      │            │
                    │  Timer Running  │            │
                    └────────┬────────┘            │
                             │                     │
        ┌────────────────────┼────────────────┐    │
        │                    │                │    │
        ▼                    ▼                ▼    │
┌──────────────┐    ┌────────────────┐  ┌─────────┐
│Duration      │    │Stop Button     │  │Safety   │
│Complete      │    │Pressed         │  │Timeout  │
└──────┬───────┘    └────────┬───────┘  └────┬────┘
       │                     │               │
       └─────────────────────┼───────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │   STOPPING      │
                    │  Close Valve    │
                    │  Save State     │
                    └────────┬────────┘
                             │
                             └─────────────────────┘
```

## Class Diagrams

### IrrigationController

```
┌─────────────────────────────────────────┐
│      IrrigationController               │
├─────────────────────────────────────────┤
│ - _schedules[MAX_SCHEDULES]             │
│ - _status: SystemStatus                 │
│ - _currentTime: time_t                  │
│ - _hasValidTime: bool                   │
│ - _irrigationStartMillis: ulong         │
│ - _currentDurationMinutes: uint16_t     │
├─────────────────────────────────────────┤
│ + begin(): bool                         │
│ + update(): void                        │
│ + startIrrigation(duration): void       │
│ + stopIrrigation(): void                │
│ + isIrrigating(): bool                  │
│ + addSchedule(...): bool                │
│ + removeSchedule(index): bool           │
│ + getSchedule(index): IrrigationSchedule│
│ + saveSchedules(): bool                 │
│ + loadSchedules(): bool                 │
│ + setCurrentTime(time): void            │
│ + getCurrentTime(): time_t              │
│ + getTimeRemaining(): ulong             │
│ + getNextScheduledTime(): ulong         │
├─────────────────────────────────────────┤
│ - checkSchedules(): void                │
│ - updateIrrigationState(): void         │
│ - safetyCheck(): void                   │
│ - shouldRunSchedule(...): bool          │
│ - activateValve(state): void            │
└─────────────────────────────────────────┘
```

### DisplayManager

```
┌─────────────────────────────────────────┐
│         DisplayManager                  │
├─────────────────────────────────────────┤
│ - _lcd: LiquidCrystal_I2C*              │
│ - _controller: IrrigationController*    │
│ - _currentScreen: MenuScreen            │
│ - _menuIndex: uint8_t                   │
│ - _lastUpdate: ulong                    │
│ - _lastButtonState[4]: bool             │
├─────────────────────────────────────────┤
│ + begin(): bool                         │
│ + update(): void                        │
│ + showStatus(): void                    │
│ + showMenu(): void                      │
│ + showMessage(...): void                │
│ + clear(): void                         │
│ + checkButtons(): Button                │
├─────────────────────────────────────────┤
│ - initButtons(): void                   │
│ - updateDisplay(): void                 │
│ - handleButtonPress(btn): void          │
│ - drawStatusScreen(): void              │
│ - drawMenuScreen(): void                │
│ - drawScheduleScreen(): void            │
│ - debounceButton(pin): bool             │
│ - formatTime(time): String              │
│ - formatDuration(min): String           │
└─────────────────────────────────────────┘
```

### WiFiManager

```
┌─────────────────────────────────────────┐
│          WiFiManager                    │
├─────────────────────────────────────────┤
│ - _ssid: String                         │
│ - _password: String                     │
│ - _timeSynced: bool                     │
│ - _ntpClient: NTPClient*                │
│ - _lastReconnectAttempt: ulong          │
│ - _lastTimeSync: ulong                  │
│ - _lastUpdateCheck: ulong               │
│ - _timeUpdateCallback: Function         │
├─────────────────────────────────────────┤
│ + begin(ssid, pass): bool               │
│ + update(): void                        │
│ + isConnected(): bool                   │
│ + getCurrentTime(): time_t              │
│ + isTimeSynced(): bool                  │
│ + checkForUpdates(): void               │
│ + setTimeUpdateCallback(cb): void       │
├─────────────────────────────────────────┤
│ - connectWiFi(): void                   │
│ - setupOTA(): void                      │
│ - syncTime(): void                      │
│ - checkGitHubVersion(...): bool         │
│ - downloadFirmware(url): bool           │
│ - performOTA(): void                    │
└─────────────────────────────────────────┘
```

### HomeAssistantIntegration

```
┌─────────────────────────────────────────┐
│    HomeAssistantIntegration             │
├─────────────────────────────────────────┤
│ - _controller: IrrigationController*    │
│ - _mqttClient: PubSubClient*            │
│ - _broker: String                       │
│ - _port: uint16_t                       │
│ - _user: String                         │
│ - _password: String                     │
│ - _lastReconnectAttempt: ulong          │
│ - _lastStatusUpdate: ulong              │
├─────────────────────────────────────────┤
│ + begin(broker, port, ...): bool        │
│ + update(): void                        │
│ + isConnected(): bool                   │
│ + publishState(): void                  │
│ + publishStatus(): void                 │
│ + publishSchedule(): void               │
│ + publishDiscovery(): void              │
├─────────────────────────────────────────┤
│ - connectMQTT(): void                   │
│ - subscribe(): void                     │
│ - handleMQTTMessage(...): void          │
│ - buildTopic(suffix): String            │
└─────────────────────────────────────────┘
```

## Timing Diagrams

### Schedule Check and Execution

```
Time:  0s    30s   60s   90s   120s  150s  180s  ...
       │     │     │     │     │     │     │
Check: ●─────●─────●─────●─────●─────●─────●     (every 30s)
       │           │           │
       │           │           └─► Match found!
       │           │               (6:00 AM)
       │           │
       │           └─► No match   ┌──────────────┐
       │               (5:59 AM)  │ Irrigation   │
       │                          │ Starts       │
       └─► No match               └──────┬───────┘
           (5:58 AM)                     │
                                        │
                                    [30 minutes]
                                        │
                                        ▼
                                   ┌────────────┐
                                   │ Irrigation │
                                   │ Stops      │
                                   └────────────┘
```

### Button Debounce Logic

```
Physical Button Signal:
         ┌─┐ ┌┐
────┐    │ │ │└─────────────────
    └────┘ └─┘
    ^
    Noisy contact bounce

Debounced Signal:
         ┌──────────────────────
────┐    │
    └────┘
    ^    ^
    │    └─ Valid press detected after 50ms
    └─ Initial press
```

### MQTT Communication Flow

```
ESP32                          MQTT Broker              Home Assistant
  │                                 │                          │
  │──── CONNECT ──────────────────▶│                          │
  │                                 │                          │
  │◀─── CONNACK ───────────────────│                          │
  │                                 │                          │
  │──── PUBLISH (discovery) ──────▶│                          │
  │                                 │──── PUBLISH ───────────▶│
  │                                 │                          │
  │                                 │                      [Entities]
  │                                 │                      [Created]
  │                                 │                          │
  │──── PUBLISH (state=OFF) ──────▶│                          │
  │                                 │──── PUBLISH ───────────▶│
  │                                 │                          │
  │                                 │◀─── SUBSCRIBE ──────────│
  │                                 │     (command topic)      │
  │                                 │                          │
  │                                 │                     [User clicks]
  │                                 │                     [ON button]
  │                                 │                          │
  │                                 │◀─── PUBLISH ────────────│
  │◀─── PUBLISH (cmd=ON) ──────────│     (command=ON)         │
  │                                 │                          │
  [Start Irrigation]                │                          │
  │                                 │                          │
  │──── PUBLISH (state=ON) ────────▶│                          │
  │                                 │──── PUBLISH ───────────▶│
  │                                 │                          │
                                                          [UI Updates]
```

## Memory Layout

### Flash Memory (4MB typical)

```
┌──────────────────────────────────┐ 0x000000
│      Bootloader (64KB)           │
├──────────────────────────────────┤ 0x010000
│      Partition Table (4KB)       │
├──────────────────────────────────┤ 0x011000
│      NVS (20KB)                  │
├──────────────────────────────────┤ 0x016000
│      OTA Data (8KB)              │
├──────────────────────────────────┤ 0x018000
│      App0 (1.2MB)                │
│      (Current Firmware)          │
├──────────────────────────────────┤ 0x148000
│      App1 (1.2MB)                │
│      (OTA Update Storage)        │
├──────────────────────────────────┤ 0x278000
│      SPIFFS (1.5MB)              │
│      - config.json               │
│      - schedule.json             │
│      - irrigation.log            │
└──────────────────────────────────┘ 0x3FFFFF
```

### RAM Usage Estimate

```
Component              RAM Usage
────────────────────────────────────
ESP32 System           ~80KB
WiFi/BT Stack          ~40KB
Arduino Core           ~20KB
────────────────────────────────────
IrrigationController   ~2KB
DisplayManager         ~4KB
WiFiManager            ~8KB
HomeAssistant          ~12KB
────────────────────────────────────
LCD Buffer             ~2KB
MQTT Buffer            ~1KB
JSON Buffers           ~4KB
Stack/Heap             ~50KB
────────────────────────────────────
Total                  ~223KB / 520KB
Free                   ~297KB (57%)
```

## Power Consumption

### Typical Operating Modes

```
Mode                Current Draw    Duration      Energy
──────────────────────────────────────────────────────────
Boot/Init           ~180mA         ~5s           0.25Wh
Idle (WiFi on)      ~80mA          Variable      Continuous
Irrigating          ~85mA          30-120min     Variable
WiFi scanning       ~120mA         ~2s           Short bursts
MQTT transmit       ~180mA         ~100ms        Minimal
Deep sleep          ~10µA          N/A           (future)
──────────────────────────────────────────────────────────

Average daily consumption (with WiFi): ~2Wh (@ 5V = 400mAh)
```

### Valve Power (Separate Circuit)

```
12V Solenoid Valve:
- Inrush: ~1.5A (18W) for ~100ms
- Hold: ~400mA (4.8W) continuous
- Daily energy: ~2-4Wh (depending on schedule)
```

## Error Handling

### Error Recovery Matrix

```
Error Condition          Detection                Recovery Action                Auto-Recover
──────────────────────────────────────────────────────────────────────────────────────────────
WiFi Disconnect          WiFi.status()            Retry connection every 30s     Yes
MQTT Disconnect          !client.connected()      Reconnect every 5s             Yes
NTP Sync Fail            !ntpClient.update()      Retry next hour                Yes
SPIFFS Mount Fail        !SPIFFS.begin()          Use default values             No
Schedule Load Fail       JSON error               Use defaults, log error        No
Valve Stuck On           Safety timeout           Force valve off, alert         Yes
OTA Download Fail        HTTP error               Skip update, retry next day    Yes
Button Stuck             No state change          Ignore, timeout                Yes
LCD Init Fail            No I2C ACK               Continue without display       No
Time Not Synced          _hasValidTime==false     Skip schedules, allow manual   Yes
Memory Low               ESP.getFreeHeap()        Skip non-critical functions    Auto
────────────────────────────────────────────────────────────────────────────────────────────
```

## Performance Metrics

### Response Times

```
Operation                    Typical Time    Maximum Time
────────────────────────────────────────────────────────────
Button press → Action        <100ms          <200ms
Manual start → Valve on      <50ms           <100ms
Schedule trigger → Start     <1s             <5s
MQTT command → Action        <500ms          <2s
OTA check                    ~2s             ~10s
OTA download & install       ~30s            ~2min
NTP sync                     <1s             <5s
Display update               ~50ms           ~200ms
SPIFFS write                 ~100ms          ~500ms
────────────────────────────────────────────────────────────
```

### Network Traffic

```
Event                  Frequency        Data Size    Daily Total
────────────────────────────────────────────────────────────────
MQTT Status Update     Every 60s        ~200 bytes   ~280KB
MQTT State Change      On event         ~50 bytes    Minimal
NTP Sync              Every hour        ~80 bytes    ~2KB
OTA Check             Daily             ~1KB         ~1KB
Home Assistant Disc.   On connect       ~2KB         ~2KB
────────────────────────────────────────────────────────────────
Total daily network:                                  ~285KB
```

## Security Considerations

### Current Implementation

```
✓ WiFi password stored in flash (encrypted by ESP32)
✓ MQTT credentials in flash
✓ OTA password protection
✓ No exposed web interface (less attack surface)
✓ Local operation capability (offline mode)
✗ MQTT not encrypted (TODO: Add TLS)
✗ OTA not signed (TODO: Add signature verification)
✗ No authentication for physical buttons
```

### Recommended Enhancements

1. **Enable MQTT TLS**
   ```cpp
   WiFiClientSecure secureClient;
   secureClient.setCACert(ca_cert);
   PubSubClient mqttClient(secureClient);
   ```

2. **Implement OTA Signature Verification**
   - Sign firmware with private key
   - Verify with public key before flashing

3. **Add Physical Tamper Detection**
   - Reed switch on enclosure
   - Alert via Home Assistant

4. **Implement Rate Limiting**
   - Limit MQTT command frequency
   - Prevent DoS on button inputs

## Testing Checklist

### Unit Tests

- [ ] Schedule time matching
- [ ] Weekday bitmask logic
- [ ] Duration calculations
- [ ] Button debounce timing
- [ ] JSON parsing/generation
- [ ] Time conversion functions

### Integration Tests

- [ ] WiFi connection and recovery
- [ ] MQTT publish/subscribe
- [ ] NTP synchronization
- [ ] OTA update process
- [ ] SPIFFS read/write
- [ ] LCD display output
- [ ] Button input handling

### System Tests

- [ ] Complete irrigation cycle
- [ ] Schedule execution
- [ ] Manual override
- [ ] Safety timeout
- [ ] Power cycle recovery
- [ ] Network loss recovery
- [ ] Home Assistant integration
- [ ] OTA firmware update

### Stress Tests

- [ ] 24-hour continuous operation
- [ ] Rapid button presses
- [ ] WiFi connect/disconnect cycles
- [ ] Multiple schedule conflicts
- [ ] SPIFFS wear testing
- [ ] Memory leak detection

## Troubleshooting Guide

### Common Issues and Solutions

#### Issue: LCD shows garbage characters
**Cause**: Wrong I2C address or bad connection
**Solution**:
```cpp
// Run I2C scanner to find correct address
Wire.begin(LCD_SDA, LCD_SCL);
for (uint8_t addr = 0x20; addr < 0x28; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
        Serial.printf("Found I2C device at 0x%02X\n", addr);
    }
}
```

#### Issue: Schedules not triggering
**Diagnostics**:
1. Check time sync: `wifiManager->isTimeSynced()`
2. Verify schedule enabled: LCD menu
3. Check weekday bitmask
4. Monitor serial debug output

#### Issue: High memory usage / crashes
**Solution**:
```cpp
// Add to loop() for monitoring
if (ESP.getFreeHeap() < 50000) {
    DEBUG_PRINTF("WARNING: Low memory: %d bytes\n", ESP.getFreeHeap());
}
```

#### Issue: MQTT not auto-discovering
**Checklist**:
- [ ] MQTT broker running and accessible
- [ ] Discovery prefix matches HA config
- [ ] Unique device ID
- [ ] ESP32 connected to MQTT
- [ ] Check HA MQTT integration logs

## Appendix

### Bill of Materials (BOM)

```
Component                    Qty    Est. Cost
─────────────────────────────────────────────
ESP32 Development Board      1      $8-12
5V Relay Module (1-channel)  1      $2-4
20x4 I2C LCD Display         1      $8-12
Push Buttons (tactile)       4      $2
12V Solenoid Valve           1      $10-20
12V Power Supply (2A)        1      $8-12
5V Power Supply or Buck Conv 1      $3-8
Enclosure (waterproof)       1      $15-25
Wires, connectors, misc      -      $10
─────────────────────────────────────────────
Total                               $66-$105
```

### Recommended Tools

- Soldering iron and solder
- Multimeter
- Wire strippers
- Screwdriver set
- Heat shrink tubing
- USB cable for programming

### Useful Commands

```bash
# PlatformIO
pio run -t clean          # Clean build
pio run -t upload         # Upload firmware
pio run -t uploadfs       # Upload filesystem
pio device monitor        # Serial monitor
pio device list           # List serial ports

# ESP32 specific
esptool.py chip_id        # Get chip ID
esptool.py flash_id       # Get flash info
esptool.py erase_flash    # Erase everything

# Git (for OTA)
git tag v1.0.1            # Tag version
git push --tags           # Push tags
```

### Reference Links

- ESP32 Pinout: https://randomnerdtutorials.com/esp32-pinout-reference-gpios/
- PlatformIO Docs: https://docs.platformio.org/
- Home Assistant MQTT: https://www.home-assistant.io/integrations/mqtt/
- ArduinoJSON: https://arduinojson.org/
- ESP32 Arduino Core: https://github.com/espressif/arduino-esp32

---

**Document Version**: 1.0
**Last Updated**: 2024-01-15
**Author**: Your Name
