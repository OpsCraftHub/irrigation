#ifndef IRRIGATION_CONTROLLER_H
#define IRRIGATION_CONTROLLER_H

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"

// Callback for routing valve commands to remote nodes
typedef void (*RemoteValveCallback)(uint8_t channel, bool state, uint16_t duration);

class IrrigationController {
public:
    IrrigationController();
    ~IrrigationController();

    // Initialization
    bool begin();

    // Main update loop - call this frequently
    void update();

    // Manual control
    void startIrrigation(uint8_t channel = 1, uint16_t durationMinutes = DEFAULT_DURATION_MINUTES, bool manual = true);
    void stopIrrigation(uint8_t channel = 0);  // 0 = stop all channels
    bool isIrrigating() const { return _status.irrigating; }
    bool isChannelIrrigating(uint8_t channel) const;
    bool isManualMode() const { return _status.manualMode; }
    void setManualMode(bool manual) { _status.manualMode = manual; }
    void setSystemEnabled(bool enabled);
    bool isSystemEnabled() const { return _systemEnabled; }

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
    unsigned long getNextScheduledTime(uint8_t* nextChannel = nullptr, uint8_t* nextIndex = nullptr) const;
    void skipSchedule(uint8_t index);    // Skip the next run of a specific schedule
    void unskipSchedule(uint8_t index);  // Cancel a skip
    bool isScheduleSkipped(uint8_t index) const;
    uint8_t getChannelPin(uint8_t channel) const;

    // Channel settings
    bool isChannelInverted(uint8_t channel) const;
    void setChannelInverted(uint8_t channel, bool inverted);
    bool isChannelEnabled(uint8_t channel) const;
    void setChannelEnabled(uint8_t channel, bool enabled);
    bool saveChannelSettings();
    bool loadChannelSettings();

    // Remote valve support (ESP-NOW virtual channels)
    void setRemoteValveCallback(RemoteValveCallback cb) { _remoteValveCallback = cb; }
    void setRemoteChannelStatus(uint8_t channel, bool irrigating, uint16_t remainingSec);

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
    void activateValve(uint8_t channel, bool state, bool manual = true);
    int8_t findFreeScheduleSlot() const;

    // Member variables
    IrrigationSchedule _schedules[MAX_SCHEDULES];
    SystemStatus _status;
    time_t _currentTime;
    bool _hasValidTime;
    unsigned long _lastScheduleCheck;
    unsigned long _irrigationStartMillis;
    uint16_t _currentDurationMinutes;
    RemoteValveCallback _remoteValveCallback = nullptr;
    bool _channelEnabled[MAX_CHANNELS];
    bool _systemEnabled;
    time_t _skipUntil[MAX_SCHEDULES];  // RAM-only: skip schedule until this time
};

#endif // IRRIGATION_CONTROLLER_H
