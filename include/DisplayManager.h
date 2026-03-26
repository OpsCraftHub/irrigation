#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"
#include "IrrigationController.h"

enum MenuScreen {
    SCREEN_STATUS,        // Main status display
    SCREEN_MENU_MAIN,     // Main menu
    SCREEN_SCHEDULE,      // Schedule configuration
    SCREEN_DURATION,      // Duration setting
    SCREEN_MANUAL,        // Manual control
    SCREEN_SETTINGS,      // System settings
    SCREEN_PAIR_REQUEST   // Incoming pair request approval
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

    // Pair request screen
    void showPairRequest(const char* nodeId, const char* name);
    void clearPairRequest();

    // Pair response callback — called when user accepts/rejects via buttons
    typedef void (*PairResponseCallback)(bool accepted);
    void setPairResponseCallback(PairResponseCallback cb) { _pairResponseCallback = cb; }

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
    void drawPairRequestScreen();
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

    // Pair request state
    PairResponseCallback _pairResponseCallback;
    bool _pairPending;
    char _pairNodeId[12];
    char _pairName[16];
};

#endif // DISPLAY_MANAGER_H
