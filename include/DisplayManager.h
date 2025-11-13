#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"
#include "IrrigationController.h"

enum MenuScreen {
    SCREEN_STATUS,      // Main status display
    SCREEN_MENU_MAIN,   // Main menu
    SCREEN_SCHEDULE,    // Schedule configuration
    SCREEN_DURATION,    // Duration setting
    SCREEN_MANUAL,      // Manual control
    SCREEN_SETTINGS     // System settings
};

enum Button {
    BTN_NONE,
    BTN_START_PRESSED,
    BTN_STOP_PRESSED,
    BTN_NEXT_PRESSED,
    BTN_SELECT_PRESSED
};

class DisplayManager {
public:
    DisplayManager(IrrigationController* controller);
    ~DisplayManager();

    // Initialization
    bool begin();

    // Main update loop
    void update();

    // Display control
    void showStatus();
    void showMenu();
    void showMessage(const char* line1, const char* line2 = nullptr,
                     const char* line3 = nullptr, const char* line4 = nullptr);
    void clear();

    // Button handling
    Button checkButtons();

private:
    // Internal methods
    void initButtons();
    void updateDisplay();
    void handleButtonPress(Button btn);
    void drawStatusScreen();
    void drawMenuScreen();
    void drawScheduleScreen();
    void drawDurationScreen();
    void drawManualScreen();
    void drawSettingsScreen();
    String formatTime(time_t time);
    String formatDuration(unsigned long minutes);
    bool debounceButton(uint8_t pin);

    // Member variables
    LiquidCrystal_I2C* _lcd;
    IrrigationController* _controller;
    MenuScreen _currentScreen;
    uint8_t _menuIndex;
    uint8_t _editValue;
    bool _editMode;
    unsigned long _lastUpdate;
    unsigned long _lastButtonPress[4];
    bool _lastButtonState[4];
};

#endif // DISPLAY_MANAGER_H
