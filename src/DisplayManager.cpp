#include "DisplayManager.h"
#include <time.h>

DisplayManager::DisplayManager(IrrigationController* controller)
    : _lcd(nullptr),
      _controller(controller),
      _currentScreen(SCREEN_STATUS),
      _menuIndex(0),
      _editValue(0),
      _editMode(false),
      _lastUpdate(0) {

    for (int i = 0; i < 4; i++) {
        _lastButtonPress[i] = 0;
        _lastButtonState[i] = HIGH;
    }
}

DisplayManager::~DisplayManager() {
    if (_lcd != nullptr) {
        delete _lcd;
    }
}

bool DisplayManager::begin() {
    DEBUG_PRINTLN("DisplayManager: Initializing...");

    // Initialize I2C
    Wire.begin(LCD_SDA, LCD_SCL);

    // Check if LCD is present by scanning I2C
    Wire.beginTransmission(LCD_ADDRESS);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
        DEBUG_PRINTLN("DisplayManager: LCD found, initializing...");
        // Initialize LCD
        _lcd = new LiquidCrystal_I2C(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
        _lcd->init();
        _lcd->backlight();
        _lcd->clear();

        // Show startup message
        showMessage("Irrigation System", "Initializing...", "Version " VERSION, "");

        delay(2000); // Show startup message briefly
    } else {
        DEBUG_PRINTLN("DisplayManager: LCD not found, continuing without display");
        _lcd = nullptr;
    }

    // Initialize buttons
    initButtons();

    DEBUG_PRINTLN("DisplayManager: Initialized successfully");
    return true;
}

void DisplayManager::initButtons() {
    pinMode(BTN_START, INPUT_PULLUP);
    pinMode(BTN_STOP, INPUT_PULLUP);
    pinMode(BTN_NEXT, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
}

void DisplayManager::update() {
    unsigned long currentMillis = millis();

    // Update display periodically
    if (currentMillis - _lastUpdate >= DISPLAY_UPDATE_INTERVAL) {
        _lastUpdate = currentMillis;
        updateDisplay();
    }

    // Check for button presses
    Button btn = checkButtons();
    if (btn != BTN_NONE) {
        handleButtonPress(btn);
    }
}

void DisplayManager::updateDisplay() {
    switch (_currentScreen) {
        case SCREEN_STATUS:
            drawStatusScreen();
            break;
        case SCREEN_MENU_MAIN:
            drawMenuScreen();
            break;
        case SCREEN_SCHEDULE:
            drawScheduleScreen();
            break;
        case SCREEN_DURATION:
            drawDurationScreen();
            break;
        case SCREEN_MANUAL:
            drawManualScreen();
            break;
        case SCREEN_SETTINGS:
            drawSettingsScreen();
            break;
    }
}

void DisplayManager::drawStatusScreen() {
    if (!_lcd) return;

    SystemStatus status = _controller->getStatus();
    _lcd->clear();

    // Line 1: WiFi and MQTT status
    _lcd->setCursor(0, 0);
    _lcd->print("WiFi:");
    _lcd->print(status.wifiConnected ? "OK " : "-- ");
    _lcd->print("MQTT:");
    _lcd->print(status.mqttConnected ? "OK" : "--");

    // Line 2: Current status
    _lcd->setCursor(0, 1);
    if (status.irrigating) {
        _lcd->print("IRRIGATING ");
        if (status.manualMode) {
            _lcd->print("(MAN)");
        } else {
            _lcd->print("(SCH)");
        }
    } else {
        _lcd->print("IDLE            ");
    }

    // Line 3: Time remaining or last run
    _lcd->setCursor(0, 2);
    if (status.irrigating) {
        unsigned long remaining = _controller->getTimeRemaining();
        _lcd->print("Remaining: ");
        _lcd->print(formatDuration(remaining));
    } else if (status.lastIrrigationTime > 0) {
        _lcd->print("Last: ");
        _lcd->print(formatTime(status.lastIrrigationTime));
    } else {
        _lcd->print("No recent run");
    }

    // Line 4: Next scheduled run
    _lcd->setCursor(0, 3);
    unsigned long nextTime = _controller->getNextScheduledTime();
    if (nextTime > 0) {
        _lcd->print("Next: ");
        _lcd->print(formatTime(nextTime));
    } else {
        _lcd->print("No schedules");
    }
}

void DisplayManager::drawMenuScreen() {
    if (!_lcd) return;

    _lcd->clear();
    _lcd->setCursor(0, 0);
    _lcd->print("== MAIN MENU ==");

    const char* menuItems[] = {
        "1. Manual Control",
        "2. View Schedules",
        "3. Edit Schedule",
        "4. System Info"
    };

    for (int i = 0; i < 3 && (i + _menuIndex) < 4; i++) {
        _lcd->setCursor(0, i + 1);
        if (i == 0) {
            _lcd->print("> ");
        } else {
            _lcd->print("  ");
        }
        _lcd->print(menuItems[i + _menuIndex]);
    }
}

void DisplayManager::drawScheduleScreen() {
    if (!_lcd) return;

    _lcd->clear();
    _lcd->setCursor(0, 0);
    _lcd->print("== SCHEDULES ==");

    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count;
    _controller->getSchedules(schedules, count);

    for (int i = 0; i < 3 && (i + _menuIndex) < count; i++) {
        int idx = i + _menuIndex;
        _lcd->setCursor(0, i + 1);

        if (i == 0) {
            _lcd->print(">");
        } else {
            _lcd->print(" ");
        }

        _lcd->print(idx + 1);
        _lcd->print(".");

        if (schedules[idx].enabled) {
            char timeStr[10];
            sprintf(timeStr, "%02d:%02d", schedules[idx].hour, schedules[idx].minute);
            _lcd->print(timeStr);
            _lcd->print(" ");
            _lcd->print(schedules[idx].durationMinutes);
            _lcd->print("m");
        } else {
            _lcd->print("Disabled");
        }
    }
}

void DisplayManager::drawDurationScreen() {
    if (!_lcd) return;

    _lcd->clear();
    _lcd->setCursor(0, 0);
    _lcd->print("Set Duration");

    _lcd->setCursor(0, 2);
    _lcd->print("Minutes: ");
    _lcd->print(_editValue);

    _lcd->setCursor(0, 3);
    _lcd->print("SELECT to confirm");
}

void DisplayManager::drawManualScreen() {
    if (!_lcd) return;

    _lcd->clear();
    _lcd->setCursor(0, 0);
    _lcd->print("== MANUAL MODE ==");

    SystemStatus status = _controller->getStatus();

    if (status.irrigating) {
        _lcd->setCursor(0, 1);
        _lcd->print("Status: RUNNING");

        _lcd->setCursor(0, 2);
        _lcd->print("Time left: ");
        _lcd->print(formatDuration(_controller->getTimeRemaining()));

        _lcd->setCursor(0, 3);
        _lcd->print("STOP to cancel");
    } else {
        _lcd->setCursor(0, 1);
        _lcd->print("Status: IDLE");

        _lcd->setCursor(0, 3);
        _lcd->print("START to begin");
    }
}

void DisplayManager::drawSettingsScreen() {
    if (!_lcd) return;

    _lcd->clear();
    _lcd->setCursor(0, 0);
    _lcd->print("== SYSTEM INFO ==");

    SystemStatus status = _controller->getStatus();

    _lcd->setCursor(0, 1);
    _lcd->print("Ver: ");
    _lcd->print(VERSION);

    _lcd->setCursor(0, 2);
    if (_controller->hasValidTime()) {
        _lcd->print(formatTime(_controller->getCurrentTime()));
    } else {
        _lcd->print("Time: Not synced");
    }

    _lcd->setCursor(0, 3);
    if (!status.lastError.isEmpty()) {
        _lcd->print("Err:");
        _lcd->print(status.lastError.substring(0, 15));
    } else {
        _lcd->print("Status: OK");
    }
}

Button DisplayManager::checkButtons() {
    if (debounceButton(BTN_START)) {
        return BTN_START_PRESSED;
    }
    if (debounceButton(BTN_STOP)) {
        return BTN_STOP_PRESSED;
    }
    if (debounceButton(BTN_NEXT)) {
        return BTN_NEXT_PRESSED;
    }
    if (debounceButton(BTN_SELECT)) {
        return BTN_SELECT_PRESSED;
    }
    return BTN_NONE;
}

bool DisplayManager::debounceButton(uint8_t pin) {
    int buttonIndex = -1;

    if (pin == BTN_START) buttonIndex = 0;
    else if (pin == BTN_STOP) buttonIndex = 1;
    else if (pin == BTN_NEXT) buttonIndex = 2;
    else if (pin == BTN_SELECT) buttonIndex = 3;

    if (buttonIndex < 0) return false;

    bool currentState = digitalRead(pin);
    unsigned long currentTime = millis();

    // Button pressed (LOW due to pullup)
    if (currentState == LOW && _lastButtonState[buttonIndex] == HIGH) {
        if (currentTime - _lastButtonPress[buttonIndex] > BUTTON_DEBOUNCE_MS) {
            _lastButtonPress[buttonIndex] = currentTime;
            _lastButtonState[buttonIndex] = LOW;
            return true;
        }
    }

    // Button released
    if (currentState == HIGH) {
        _lastButtonState[buttonIndex] = HIGH;
    }

    return false;
}

void DisplayManager::handleButtonPress(Button btn) {
    SystemStatus status = _controller->getStatus();

    switch (btn) {
        case BTN_START_PRESSED:
            if (_currentScreen == SCREEN_STATUS) {
                // Quick start from status screen
                _controller->startIrrigation(DEFAULT_DURATION_MINUTES);
            } else if (_currentScreen == SCREEN_MANUAL) {
                _controller->startIrrigation(DEFAULT_DURATION_MINUTES);
            }
            break;

        case BTN_STOP_PRESSED:
            if (status.irrigating) {
                _controller->stopIrrigation();
            }
            // Return to status screen
            _currentScreen = SCREEN_STATUS;
            _menuIndex = 0;
            break;

        case BTN_NEXT_PRESSED:
            if (_currentScreen == SCREEN_STATUS) {
                // Enter menu
                _currentScreen = SCREEN_MENU_MAIN;
                _menuIndex = 0;
            } else {
                // Navigate through menu
                _menuIndex++;
                if (_menuIndex > 3) {
                    _menuIndex = 0;
                }
            }
            break;

        case BTN_SELECT_PRESSED:
            if (_currentScreen == SCREEN_MENU_MAIN) {
                // Select menu item
                switch (_menuIndex) {
                    case 0:
                        _currentScreen = SCREEN_MANUAL;
                        break;
                    case 1:
                        _currentScreen = SCREEN_SCHEDULE;
                        _menuIndex = 0;
                        break;
                    case 2:
                        _currentScreen = SCREEN_DURATION;
                        _editValue = DEFAULT_DURATION_MINUTES;
                        break;
                    case 3:
                        _currentScreen = SCREEN_SETTINGS;
                        break;
                }
            } else {
                // Return to main menu
                _currentScreen = SCREEN_MENU_MAIN;
                _menuIndex = 0;
            }
            break;

        default:
            break;
    }
}

void DisplayManager::showStatus() {
    _currentScreen = SCREEN_STATUS;
    updateDisplay();
}

void DisplayManager::showMenu() {
    _currentScreen = SCREEN_MENU_MAIN;
    _menuIndex = 0;
    updateDisplay();
}

void DisplayManager::showMessage(const char* line1, const char* line2,
                                 const char* line3, const char* line4) {
    if (!_lcd) return;

    _lcd->clear();

    if (line1) {
        _lcd->setCursor(0, 0);
        _lcd->print(line1);
    }
    if (line2) {
        _lcd->setCursor(0, 1);
        _lcd->print(line2);
    }
    if (line3) {
        _lcd->setCursor(0, 2);
        _lcd->print(line3);
    }
    if (line4) {
        _lcd->setCursor(0, 3);
        _lcd->print(line4);
    }
}

void DisplayManager::clear() {
    if (!_lcd) return;

    _lcd->clear();
}

String DisplayManager::formatTime(time_t time) {
    if (time == 0) {
        return "N/A";
    }

    struct tm timeinfo;
    localtime_r(&time, &timeinfo);

    char buffer[20];
    sprintf(buffer, "%02d/%02d %02d:%02d",
            timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min);

    return String(buffer);
}

String DisplayManager::formatDuration(unsigned long minutes) {
    if (minutes >= 60) {
        return String(minutes / 60) + "h" + String(minutes % 60) + "m";
    } else {
        return String(minutes) + "min";
    }
}
