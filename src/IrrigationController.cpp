#include "IrrigationController.h"
#include <time.h>

IrrigationController::IrrigationController()
    : _currentTime(0),
      _hasValidTime(false),
      _lastScheduleCheck(0),
      _irrigationStartMillis(0),
      _currentDurationMinutes(0) {

    // Initialize status
    memset(&_status, 0, sizeof(SystemStatus));

    // Initialize schedules
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        _schedules[i].enabled = false;
        _schedules[i].channel = 1;  // Default to channel 1
        _schedules[i].hour = 0;
        _schedules[i].minute = 0;
        _schedules[i].durationMinutes = DEFAULT_DURATION_MINUTES;
        _schedules[i].weekdays = 0x7F; // All days
    }
}

IrrigationController::~IrrigationController() {
    stopIrrigation();
}

bool IrrigationController::begin() {
    DEBUG_PRINTLN("IrrigationController: Initializing...");

    // Configure all channel pins
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        pinMode(CHANNEL_PINS[i], OUTPUT);
        digitalWrite(CHANNEL_PINS[i], LOW);
        _status.channelIrrigating[i] = false;
        _status.channelStartTime[i] = 0;
        _status.channelDuration[i] = 0;
    }

    // Initialize SPIFFS for storage
    if (!SPIFFS.begin(true)) {
        DEBUG_PRINTLN("IrrigationController: SPIFFS mount failed");
        _status.lastError = "SPIFFS failed";
        return false;
    }

    // Load schedules from storage
    if (!loadSchedules()) {
        DEBUG_PRINTLN("IrrigationController: No saved schedules, using defaults");
    }

    DEBUG_PRINTLN("IrrigationController: Initialized successfully");
    return true;
}

void IrrigationController::update() {
    unsigned long currentMillis = millis();

    // Update irrigation state
    updateIrrigationState();

    // Safety check
    safetyCheck();

    // Check schedules periodically
    if (currentMillis - _lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
        _lastScheduleCheck = currentMillis;
        if (_hasValidTime && !_status.manualMode) {
            checkSchedules();
        }
    }
}

void IrrigationController::startIrrigation(uint8_t channel, uint16_t durationMinutes) {
    // Validate channel
    if (channel < 1 || channel > MAX_CHANNELS) {
        DEBUG_PRINTF("Invalid channel: %d\n", channel);
        return;
    }

    // Validate duration
    if (durationMinutes < MIN_DURATION_MINUTES) {
        durationMinutes = MIN_DURATION_MINUTES;
    }
    if (durationMinutes > MAX_DURATION_MINUTES) {
        durationMinutes = MAX_DURATION_MINUTES;
    }

    DEBUG_PRINTF("IrrigationController: Starting irrigation on channel %d for %d minutes\n", channel, durationMinutes);

    uint8_t idx = channel - 1;  // Convert to 0-based index

    _status.channelIrrigating[idx] = true;
    _status.channelStartTime[idx] = millis();
    _status.channelDuration[idx] = durationMinutes;
    _status.irrigating = true;
    _status.irrigationStartTime = _currentTime;
    _irrigationStartMillis = millis();
    _currentDurationMinutes = durationMinutes;
    _status.currentDuration = durationMinutes;

    activateValve(channel, true);
}

void IrrigationController::stopIrrigation(uint8_t channel) {
    if (channel == 0) {
        // Stop all channels
        DEBUG_PRINTLN("IrrigationController: Stopping all channels");
        for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
            if (_status.channelIrrigating[i]) {
                _status.channelIrrigating[i] = false;
                _status.channelStartTime[i] = 0;
                _status.channelDuration[i] = 0;
                activateValve(i + 1, false);
            }
        }
        _status.irrigating = false;
        _status.manualMode = false;
        _currentDurationMinutes = 0;
        _status.currentDuration = 0;
    } else if (channel >= 1 && channel <= MAX_CHANNELS) {
        // Stop specific channel
        DEBUG_PRINTF("IrrigationController: Stopping channel %d\n", channel);
        uint8_t idx = channel - 1;
        _status.channelIrrigating[idx] = false;
        _status.channelStartTime[idx] = 0;
        _status.channelDuration[idx] = 0;
        activateValve(channel, false);

        // Update global status - check if any channel is still running
        bool anyActive = false;
        for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
            if (_status.channelIrrigating[i]) {
                anyActive = true;
                break;
            }
        }
        _status.irrigating = anyActive;

        if (!anyActive) {
            _status.manualMode = false;
            _currentDurationMinutes = 0;
            _status.currentDuration = 0;
        }
    }

    _status.lastIrrigationTime = _currentTime;
}

void IrrigationController::updateIrrigationState() {
    if (!_status.irrigating) {
        return;
    }

    unsigned long elapsedMinutes = (millis() - _irrigationStartMillis) / 60000;

    if (elapsedMinutes >= _currentDurationMinutes) {
        DEBUG_PRINTLN("IrrigationController: Irrigation cycle complete");
        stopIrrigation();
    }
}

void IrrigationController::safetyCheck() {
    if (!_status.irrigating) {
        return;
    }

    unsigned long elapsedMinutes = (millis() - _irrigationStartMillis) / 60000;

    if (elapsedMinutes >= SAFETY_TIMEOUT_MINUTES) {
        DEBUG_PRINTLN("IrrigationController: SAFETY TIMEOUT - Stopping irrigation!");
        _status.lastError = "Safety timeout triggered";
        stopIrrigation();
    }
}

void IrrigationController::checkSchedules() {
    time_t now = _currentTime;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (_schedules[i].enabled && shouldRunSchedule(_schedules[i], now)) {
            uint8_t channel = _schedules[i].channel;

            // Don't start if this channel is already running
            if (isChannelIrrigating(channel)) {
                DEBUG_PRINTF("IrrigationController: Schedule %d skipped - channel %d already running\n", i, channel);
                continue;
            }

            DEBUG_PRINTF("IrrigationController: Schedule %d triggered for channel %d\n", i, channel);
            startIrrigation(channel, _schedules[i].durationMinutes);
            // Note: Don't break - allow multiple channels to run simultaneously
        }
    }
}

bool IrrigationController::shouldRunSchedule(const IrrigationSchedule& schedule, time_t currentTime) {
    struct tm timeinfo;
    localtime_r(&currentTime, &timeinfo);

    // Check if current day is enabled
    uint8_t currentWeekday = timeinfo.tm_wday; // 0=Sunday
    if (!(schedule.weekdays & (1 << currentWeekday))) {
        return false;
    }

    // Check if current time matches schedule
    if (timeinfo.tm_hour != schedule.hour || timeinfo.tm_min != schedule.minute) {
        return false;
    }

    // Prevent running the same schedule multiple times in the same minute
    if (_status.lastIrrigationTime > 0) {
        struct tm lastRunTime;
        localtime_r(&_status.lastIrrigationTime, &lastRunTime);

        if (lastRunTime.tm_year == timeinfo.tm_year &&
            lastRunTime.tm_mon == timeinfo.tm_mon &&
            lastRunTime.tm_mday == timeinfo.tm_mday &&
            lastRunTime.tm_hour == timeinfo.tm_hour &&
            lastRunTime.tm_min == timeinfo.tm_min) {
            return false; // Already ran this minute
        }
    }

    return true;
}

void IrrigationController::activateValve(uint8_t channel, bool state) {
    if (channel < 1 || channel > MAX_CHANNELS) {
        DEBUG_PRINTF("Invalid channel: %d\n", channel);
        return;
    }

    uint8_t pin = CHANNEL_PINS[channel - 1];
    digitalWrite(pin, state ? HIGH : LOW);
    DEBUG_PRINTF("IrrigationController: Channel %d (GPIO %d) %s\n", channel, pin, state ? "ON" : "OFF");
}

// Helper methods
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

unsigned long IrrigationController::getTimeRemaining() const {
    if (!_status.irrigating) {
        return 0;
    }

    unsigned long elapsedMinutes = (millis() - _irrigationStartMillis) / 60000;
    if (elapsedMinutes >= _currentDurationMinutes) {
        return 0;
    }

    return _currentDurationMinutes - elapsedMinutes;
}

unsigned long IrrigationController::getNextScheduledTime() const {
    if (!_hasValidTime) {
        return 0;
    }

    time_t now = _currentTime;
    time_t nextTime = 0;

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!_schedules[i].enabled) {
            continue;
        }

        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Calculate next occurrence of this schedule
        timeinfo.tm_hour = _schedules[i].hour;
        timeinfo.tm_min = _schedules[i].minute;
        timeinfo.tm_sec = 0;

        time_t scheduleTime = mktime(&timeinfo);

        // If this time has passed today, check tomorrow
        if (scheduleTime <= now) {
            scheduleTime += 86400; // Add one day
        }

        // Check if this day of week is enabled
        struct tm futureTime;
        localtime_r(&scheduleTime, &futureTime);
        uint8_t weekday = futureTime.tm_wday;

        // Find next enabled weekday
        int daysToAdd = 0;
        while (!((_schedules[i].weekdays >> weekday) & 1) && daysToAdd < 7) {
            scheduleTime += 86400;
            localtime_r(&scheduleTime, &futureTime);
            weekday = futureTime.tm_wday;
            daysToAdd++;
        }

        if (daysToAdd >= 7) {
            continue; // No valid day found
        }

        if (nextTime == 0 || scheduleTime < nextTime) {
            nextTime = scheduleTime;
        }
    }

    return nextTime;
}

int8_t IrrigationController::addSchedule(uint8_t channel, uint8_t hour, uint8_t minute,
                                         uint16_t durationMinutes, uint8_t weekdays) {
    // Validate channel
    if (channel < 1 || channel > MAX_CHANNELS) {
        DEBUG_PRINTF("Invalid channel: %d\n", channel);
        return -1;
    }

    // Validate time
    if (hour > 23 || minute > 59) {
        DEBUG_PRINTLN("Invalid time");
        return -1;
    }

    // Validate duration
    if (durationMinutes < MIN_DURATION_MINUTES || durationMinutes > MAX_DURATION_MINUTES) {
        DEBUG_PRINTLN("Invalid duration");
        return -1;
    }

    // Find free slot
    int8_t index = findFreeScheduleSlot();
    if (index < 0) {
        DEBUG_PRINTLN("No free schedule slots");
        return -1;
    }

    _schedules[index].enabled = true;
    _schedules[index].channel = channel;
    _schedules[index].hour = hour;
    _schedules[index].minute = minute;
    _schedules[index].durationMinutes = durationMinutes;
    _schedules[index].weekdays = weekdays;

    DEBUG_PRINTF("IrrigationController: Schedule %d added: Ch%d at %02d:%02d for %d min\n",
                 index, channel, hour, minute, durationMinutes);

    saveSchedules();
    return index;
}

bool IrrigationController::updateSchedule(uint8_t index, uint8_t channel, uint8_t hour, uint8_t minute,
                                          uint16_t durationMinutes, uint8_t weekdays) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }

    if (channel < 1 || channel > MAX_CHANNELS) {
        return false;
    }

    if (hour > 23 || minute > 59) {
        return false;
    }

    if (durationMinutes < MIN_DURATION_MINUTES || durationMinutes > MAX_DURATION_MINUTES) {
        return false;
    }

    _schedules[index].channel = channel;
    _schedules[index].hour = hour;
    _schedules[index].minute = minute;
    _schedules[index].durationMinutes = durationMinutes;
    _schedules[index].weekdays = weekdays;

    DEBUG_PRINTF("IrrigationController: Schedule %d updated: Ch%d at %02d:%02d for %d min\n",
                 index, channel, hour, minute, durationMinutes);

    return saveSchedules();
}

bool IrrigationController::removeSchedule(uint8_t index) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }

    _schedules[index].enabled = false;
    DEBUG_PRINTF("IrrigationController: Schedule %d removed\n", index);

    return saveSchedules();
}

bool IrrigationController::enableSchedule(uint8_t index, bool enabled) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }

    _schedules[index].enabled = enabled;
    DEBUG_PRINTF("IrrigationController: Schedule %d %s\n",
                 index, enabled ? "enabled" : "disabled");

    return saveSchedules();
}

IrrigationSchedule IrrigationController::getSchedule(uint8_t index) const {
    if (index >= MAX_SCHEDULES) {
        IrrigationSchedule empty = {false, 0, 0, 0, 0};
        return empty;
    }
    return _schedules[index];
}

void IrrigationController::getSchedules(IrrigationSchedule* schedules, uint8_t& count) const {
    count = MAX_SCHEDULES;
    memcpy(schedules, _schedules, sizeof(_schedules));
}

bool IrrigationController::saveSchedules() {
    DEBUG_PRINTLN("IrrigationController: Saving schedules to SPIFFS");

    DynamicJsonDocument doc(2048);  // Increased size for 16 schedules
    JsonArray array = doc.createNestedArray("schedules");

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (_schedules[i].enabled) {  // Only save enabled schedules
            JsonObject schedule = array.createNestedObject();
            schedule["enabled"] = _schedules[i].enabled;
            schedule["channel"] = _schedules[i].channel;  // NEW
            schedule["hour"] = _schedules[i].hour;
            schedule["minute"] = _schedules[i].minute;
            schedule["duration"] = _schedules[i].durationMinutes;
            schedule["weekdays"] = _schedules[i].weekdays;
        }
    }

    File file = SPIFFS.open(SCHEDULE_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("IrrigationController: Failed to open schedule file for writing");
        return false;
    }

    serializeJson(doc, file);
    file.close();

    DEBUG_PRINTLN("IrrigationController: Schedules saved successfully");
    return true;
}

bool IrrigationController::loadSchedules() {
    DEBUG_PRINTLN("IrrigationController: Loading schedules from SPIFFS");

    if (!SPIFFS.exists(SCHEDULE_FILE)) {
        DEBUG_PRINTLN("IrrigationController: Schedule file does not exist");
        return false;
    }

    File file = SPIFFS.open(SCHEDULE_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("IrrigationController: Failed to open schedule file");
        return false;
    }

    DynamicJsonDocument doc(2048);  // Increased size for 16 schedules
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("IrrigationController: Failed to parse schedule file: %s\n", error.c_str());
        return false;
    }

    JsonArray array = doc["schedules"];
    int index = 0;

    for (JsonObject schedule : array) {
        if (index >= MAX_SCHEDULES) break;

        _schedules[index].enabled = schedule["enabled"] | false;
        _schedules[index].channel = schedule["channel"] | 1;  // NEW - default to channel 1 for backward compatibility
        _schedules[index].hour = schedule["hour"] | 0;
        _schedules[index].minute = schedule["minute"] | 0;
        _schedules[index].durationMinutes = schedule["duration"] | DEFAULT_DURATION_MINUTES;
        _schedules[index].weekdays = schedule["weekdays"] | 0x7F;

        index++;
    }

    DEBUG_PRINTF("IrrigationController: Loaded %d schedules\n", index);
    return true;
}

void IrrigationController::setCurrentTime(time_t time) {
    _currentTime = time;
    _hasValidTime = (time > 0);
}
