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
    void startIrrigation(uint8_t channel = 1, uint16_t durationMinutes = DEFAULT_DURATION_MINUTES);
    void stopIrrigation(uint8_t channel = 0);  // 0 = stop all channels
    bool isIrrigating() const { return _status.irrigating; }
    bool isChannelIrrigating(uint8_t channel) const;
    bool isManualMode() const { return _status.manualMode; }

    // Schedule management - CRUD operations
    int8_t addSchedule(uint8_t channel, uint8_t hour, uint8_t minute,
                       uint16_t durationMinutes, uint8_t weekdays);  // Returns schedule index or -1
    bool updateSchedule(uint8_t index, uint8_t channel, uint8_t hour, uint8_t minute,
                        uint16_t durationMinutes, uint8_t weekdays);
    bool removeSchedule(uint8_t index);
    bool enableSchedule(uint8_t index, bool enabled);
    IrrigationSchedule getSchedule(uint8_t index) const;
    void getSchedules(IrrigationSchedule* schedules, uint8_t& count) const;
    uint8_t getScheduleCount() const;  // Get number of active schedules

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
    void activateValve(uint8_t channel, bool state);
    uint8_t getChannelPin(uint8_t channel) const;
    int8_t findFreeScheduleSlot() const;

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
