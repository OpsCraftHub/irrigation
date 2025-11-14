# Multi-Channel Scheduler Implementation Plan

## Overview
Implementing 4-channel irrigation system with LCD-based CRUD operations for schedule management.

## Completed Changes

### 1. Config.h Updates ✅
- Added 4 channel pin definitions (GPIO 25, 4, 16, 17)
- Increased MAX_SCHEDULES from 4 to 16
- Updated IrrigationSchedule struct to include `channel` field
- Updated SystemStatus struct with per-channel tracking:
  - `bool channelIrrigating[MAX_CHANNELS]`
  - `unsigned long channelStartTime[MAX_CHANNELS]`
  - `uint16_t channelDuration[MAX_CHANNELS]`
- Added CHANNEL_PINS array for pin mapping

### 2. IrrigationController.h Updates ✅
- Updated method signatures for multi-channel support:
  - `startIrrigation(uint8_t channel, uint16_t duration)`
  - `stopIrrigation(uint8_t channel)` - channel 0 = stop all
  - `isChannelIrrigating(uint8_t channel)`
- Enhanced schedule CRUD methods:
  - `addSchedule()` - returns schedule index or -1
  - `updateSchedule()` - modify existing schedule
  - `getScheduleCount()` - count active schedules
- Added helper methods:
  - `getChannelPin(uint8_t channel)`
  - `findFreeScheduleSlot()`

## Remaining Implementation Tasks

### 3. IrrigationController.cpp - Core Logic
**File:** `src/IrrigationController.cpp`

#### A. Update begin() method
```cpp
bool IrrigationController::begin() {
    // Initialize all 4 channel pins
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        pinMode(CHANNEL_PINS[i], OUTPUT);
        digitalWrite(CHANNEL_PINS[i], LOW);
        _status.channelIrrigating[i] = false;
        _status.channelStartTime[i] = 0;
        _status.channelDuration[i] = 0;
    }
    // ... rest of initialization
}
```

#### B. Rewrite startIrrigation() for multi-channel
```cpp
void IrrigationController::startIrrigation(uint8_t channel, uint16_t durationMinutes) {
    // Validate channel (1-4)
    if (channel < 1 || channel > MAX_CHANNELS) return;

    // Validate duration
    durationMinutes = constrain(durationMinutes, MIN_DURATION_MINUTES, MAX_DURATION_MINUTES);

    uint8_t idx = channel - 1;  // Convert to 0-based index

    _status.channelIrrigating[idx] = true;
    _status.channelStartTime[idx] = millis();
    _status.channelDuration[idx] = durationMinutes;

    // Update global status
    _status.irrigating = true;

    activateValve(channel, true);
}
```

#### C. Rewrite stopIrrigation() for multi-channel
```cpp
void IrrigationController::stopIrrigation(uint8_t channel) {
    if (channel == 0) {
        // Stop all channels
        for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
            if (_status.channelIrrigating[i]) {
                _status.channelIrrigating[i] = false;
                activateValve(i + 1, false);
            }
        }
        _status.irrigating = false;
    } else if (channel >= 1 && channel <= MAX_CHANNELS) {
        // Stop specific channel
        uint8_t idx = channel - 1;
        _status.channelIrrigating[idx] = false;
        activateValve(channel, false);

        // Update global status
        bool anyActive = false;
        for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
            if (_status.channelIrrigating[i]) {
                anyActive = true;
                break;
            }
        }
        _status.irrigating = anyActive;
    }
}
```

#### D. Update activateValve() for multi-channel
```cpp
void IrrigationController::activateValve(uint8_t channel, bool state) {
    if (channel < 1 || channel > MAX_CHANNELS) return;

    uint8_t pin = CHANNEL_PINS[channel - 1];
    digitalWrite(pin, state ? HIGH : LOW);

    DEBUG_PRINTF("Channel %d (GPIO %d): %s\n", channel, pin, state ? "ON" : "OFF");
}
```

#### E. Implement new helper methods
```cpp
uint8_t IrrigationController::getChannelPin(uint8_t channel) const {
    if (channel < 1 || channel > MAX_CHANNELS) return 0;
    return CHANNEL_PINS[channel - 1];
}

bool IrrigationController::isChannelIrrigating(uint8_t channel) const {
    if (channel < 1 || channel > MAX_CHANNELS) return false;
    return _status.channelIrrigating[channel - 1];
}

int8_t IrrigationController::findFreeScheduleSlot() const {
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        if (!_schedules[i].enabled) {
            return i;
        }
    }
    return -1;  // No free slots
}

uint8_t IrrigationController::getScheduleCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        if (_schedules[i].enabled) {
            count++;
        }
    }
    return count;
}
```

#### F. Rewrite addSchedule() with auto-slot finding
```cpp
int8_t IrrigationController::addSchedule(uint8_t channel, uint8_t hour, uint8_t minute,
                                         uint16_t durationMinutes, uint8_t weekdays) {
    // Validate channel
    if (channel < 1 || channel > MAX_CHANNELS) return -1;

    // Find free slot
    int8_t index = findFreeScheduleSlot();
    if (index < 0) {
        DEBUG_PRINTLN("No free schedule slots");
        return -1;
    }

    // Create schedule
    _schedules[index].enabled = true;
    _schedules[index].channel = channel;
    _schedules[index].hour = hour;
    _schedules[index].minute = minute;
    _schedules[index].durationMinutes = durationMinutes;
    _schedules[index].weekdays = weekdays;

    saveSchedules();

    DEBUG_PRINTF("Added schedule %d: Ch%d at %02d:%02d for %dm\n",
                 index, channel, hour, minute, durationMinutes);

    return index;
}
```

#### G. Implement updateSchedule()
```cpp
bool IrrigationController::updateSchedule(uint8_t index, uint8_t channel, uint8_t hour,
                                          uint8_t minute, uint16_t durationMinutes, uint8_t weekdays) {
    if (index >= MAX_SCHEDULES) return false;
    if (channel < 1 || channel > MAX_CHANNELS) return false;

    _schedules[index].channel = channel;
    _schedules[index].hour = hour;
    _schedules[index].minute = minute;
    _schedules[index].durationMinutes = durationMinutes;
    _schedules[index].weekdays = weekdays;

    return saveSchedules();
}
```

#### H. Update checkSchedules() for multi-channel
```cpp
void IrrigationController::checkSchedules() {
    struct tm timeinfo;
    localtime_r(&_currentTime, &timeinfo);

    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        if (!_schedules[i].enabled) continue;

        if (shouldRunSchedule(_schedules[i], _currentTime)) {
            uint8_t channel = _schedules[i].channel;

            // Don't start if channel already running
            if (isChannelIrrigating(channel)) continue;

            DEBUG_PRINTF("Schedule %d triggered: Ch%d\n", i, channel);
            startIrrigation(channel, _schedules[i].durationMinutes);
        }
    }
}
```

#### I. Update updateIrrigationState() for multi-channel
```cpp
void IrrigationController::updateIrrigationState() {
    unsigned long currentMillis = millis();
    bool anyActive = false;

    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        if (_status.channelIrrigating[i]) {
            unsigned long elapsed = currentMillis - _status.channelStartTime[i];
            unsigned long durationMs = (unsigned long)_status.channelDuration[i] * 60000;

            if (elapsed >= durationMs) {
                DEBUG_PRINTF("Channel %d completed\n", i + 1);
                stopIrrigation(i + 1);
            } else {
                anyActive = true;
            }
        }
    }

    _status.irrigating = anyActive;
}
```

#### J. Update SPIFFS save/load for new schedule structure
```cpp
bool IrrigationController::saveSchedules() {
    File file = SPIFFS.open(SCHEDULE_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("Failed to open schedule file for writing");
        return false;
    }

    StaticJsonDocument<2048> doc;  // Increased size for 16 schedules
    JsonArray schedulesArray = doc.createNestedArray("schedules");

    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        if (_schedules[i].enabled) {
            JsonObject schedule = schedulesArray.createNestedObject();
            schedule["enabled"] = _schedules[i].enabled;
            schedule["channel"] = _schedules[i].channel;  // NEW
            schedule["hour"] = _schedules[i].hour;
            schedule["minute"] = _schedules[i].minute;
            schedule["duration"] = _schedules[i].durationMinutes;
            schedule["weekdays"] = _schedules[i].weekdays;
        }
    }

    serializeJson(doc, file);
    file.close();
    return true;
}

bool IrrigationController::loadSchedules() {
    File file = SPIFFS.open(SCHEDULE_FILE, "r");
    if (!file) {
        return false;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("Failed to parse schedules: %s\n", error.c_str());
        return false;
    }

    JsonArray schedulesArray = doc["schedules"];
    uint8_t index = 0;

    for (JsonObject schedule : schedulesArray) {
        if (index >= MAX_SCHEDULES) break;

        _schedules[index].enabled = schedule["enabled"] | false;
        _schedules[index].channel = schedule["channel"] | 1;  // NEW - default channel 1
        _schedules[index].hour = schedule["hour"] | 0;
        _schedules[index].minute = schedule["minute"] | 0;
        _schedules[index].durationMinutes = schedule["duration"] | DEFAULT_DURATION_MINUTES;
        _schedules[index].weekdays = schedule["weekdays"] | 0x7F;

        index++;
    }

    DEBUG_PRINTF("Loaded %d schedules\n", index);
    return true;
}
```

### 4. DisplayManager.cpp - LCD CRUD Interface
**File:** `src/DisplayManager.cpp`

#### New Menu Screens
Add enum values:
```cpp
enum MenuScreen {
    SCREEN_STATUS,
    SCREEN_MAIN_MENU,
    SCREEN_SCHEDULES_LIST,    // NEW - scrollable list
    SCREEN_SCHEDULE_EDIT,     // NEW - edit single schedule
    SCREEN_SCHEDULE_ADD,      // NEW - add new schedule
    SCREEN_MANUAL_CONTROL,
    SCREEN_SETTINGS
};
```

#### Button Mapping for Schedule Management
- **In SCREEN_SCHEDULES_LIST**:
  - NEXT: Scroll down through schedules
  - SELECT: Edit highlighted schedule
  - START: Add new schedule
  - STOP: Delete highlighted schedule

#### Display Format
```
Schedules (3/16)    [+]
Ch1 06:00 30m Daily
Ch2 18:00 20m Daily
Ch3 12:00 15m M-F  ▼
```

#### Implementation Functions Needed
```cpp
void DisplayManager::drawSchedulesList();
void DisplayManager::drawScheduleEdit(uint8_t index);
void DisplayManager::drawScheduleAdd();
void DisplayManager::handleScheduleListInput(Button btn);
void DisplayManager::handleScheduleEditInput(Button btn);
void DisplayManager::handleScheduleAddInput(Button btn);
```

### 5. Main.cpp Updates
**File:** `src/main.cpp`

Update example schedule initialization:
```cpp
void setup() {
    // ... existing init code ...

    // Add example schedules for multiple channels
    irrigationController->addSchedule(1, 6, 0, 30, 0x7F);   // Ch1: 6am, 30min, daily
    irrigationController->addSchedule(2, 18, 0, 20, 0x7F);  // Ch2: 6pm, 20min, daily
    irrigationController->addSchedule(3, 12, 0, 15, 0x3E);  // Ch3: noon, 15min, weekdays
}
```

## Testing Plan

### Unit Tests
1. Test each channel activates correct GPIO pin
2. Test simultaneous multi-channel irrigation
3. Test schedule CRUD operations (add/update/delete)
4. Test SPIFFS save/load with channel data
5. Test schedule conflict handling (same channel, overlapping times)

### Integration Tests
1. Add schedule via LCD, verify SPIFFS save
2. Delete schedule via LCD, verify removal
3. Test 4 channels running simultaneously
4. Power cycle test - verify schedules persist
5. Test LCD scrolling through 16 schedules

### Hardware Tests
1. Verify each GPIO pin controls correct relay
2. Test with actual solenoid valves
3. Verify no cross-talk between channels
4. Test current draw with all 4 channels active

## Migration Strategy

### For Existing Users
1. Backup existing `schedule.json`
2. Update firmware
3. Schedules will automatically get `channel: 1` on load
4. Add channel info to existing schedules via LCD

### JSON Migration
Old format:
```json
{"enabled": true, "hour": 6, "minute": 0, "duration": 30, "weekdays": 127}
```

New format:
```json
{"enabled": true, "channel": 1, "hour": 6, "minute": 0, "duration": 30, "weekdays": 127}
```

## Future Enhancements
- [ ] Schedule templates (copy existing schedule to new channel)
- [ ] Bulk enable/disable by channel
- [ ] Channel naming (give channels custom names)
- [ ] Flow meter integration per channel
- [ ] Water usage tracking per channel
- [ ] Home Assistant multi-channel entities

## File Modification Summary
| File | Status | Changes |
|------|--------|---------|
| `include/Config.h` | ✅ Complete | Channel pins, structures updated |
| `include/IrrigationController.h` | ✅ Complete | Method signatures updated |
| `src/IrrigationController.cpp` | 🔄 In Progress | Core logic needs multi-channel rewrite |
| `include/DisplayManager.h` | ⏳ Pending | Add new menu screens |
| `src/DisplayManager.cpp` | ⏳ Pending | Implement CRUD UI |
| `src/main.cpp` | ⏳ Pending | Update example schedules |
| `CLAUDE.md` | ⏳ Pending | Document multi-channel architecture |

## Estimated Completion Time
- Core Controller Logic: 2-3 hours
- LCD UI Implementation: 3-4 hours
- Testing & Debugging: 2-3 hours
- **Total: 7-10 hours of development**
