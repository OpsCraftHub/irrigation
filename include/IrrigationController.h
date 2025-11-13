#ifndef IRRIGATION_CONTROLLER_H
#define IRRIGATION_CONTROLLER_H

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"

class IrrigationController {
public:
    IrrigationController();
    ~IrrigationController();

    // Initialization
    bool begin();

    // Main update loop - call this frequently
    void update();

    // Manual control
    void startIrrigation(uint16_t durationMinutes = DEFAULT_DURATION_MINUTES);
    void stopIrrigation();
    bool isIrrigating() const { return _status.irrigating; }
    bool isManualMode() const { return _status.manualMode; }

    // Schedule management
    bool addSchedule(uint8_t index, uint8_t hour, uint8_t minute,
                     uint16_t durationMinutes, uint8_t weekdays);
    bool removeSchedule(uint8_t index);
    bool enableSchedule(uint8_t index, bool enabled);
    IrrigationSchedule getSchedule(uint8_t index) const;
    void getSchedules(IrrigationSchedule* schedules, uint8_t& count) const;

    // Storage
    bool saveSchedules();
    bool loadSchedules();

    // Status
    SystemStatus getStatus() const { return _status; }
    unsigned long getTimeRemaining() const;
    unsigned long getNextScheduledTime() const;

    // Time management
    void setCurrentTime(time_t time);
    time_t getCurrentTime() const { return _currentTime; }
    bool hasValidTime() const { return _hasValidTime; }

private:
    // Internal methods
    void checkSchedules();
    void updateIrrigationState();
    void safetyCheck();
    bool shouldRunSchedule(const IrrigationSchedule& schedule, time_t currentTime);
    void activateValve(bool state);

    // Member variables
    IrrigationSchedule _schedules[MAX_SCHEDULES];
    SystemStatus _status;
    time_t _currentTime;
    bool _hasValidTime;
    unsigned long _lastScheduleCheck;
    unsigned long _irrigationStartMillis;
    uint16_t _currentDurationMinutes;
};

#endif // IRRIGATION_CONTROLLER_H
