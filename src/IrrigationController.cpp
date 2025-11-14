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

    // Configure valve pin
    pinMode(VALVE_PIN, OUTPUT);
    digitalWrite(VALVE_PIN, LOW);

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

void IrrigationController::startIrrigation(uint16_t durationMinutes) {
    // Validate duration
    if (durationMinutes < MIN_DURATION_MINUTES) {
        durationMinutes = MIN_DURATION_MINUTES;
    }
    if (durationMinutes > MAX_DURATION_MINUTES) {
        durationMinutes = MAX_DURATION_MINUTES;
    }

    DEBUG_PRINTF("IrrigationController: Starting irrigation for %d minutes\n", durationMinutes);

    _status.irrigating = true;
    _status.irrigationStartTime = _currentTime;
    _irrigationStartMillis = millis();
    _currentDurationMinutes = durationMinutes;
    _status.currentDuration = durationMinutes;

    activateValve(true);
}

void IrrigationController::stopIrrigation() {
    if (!_status.irrigating) {
        return;
    }

    DEBUG_PRINTLN("IrrigationController: Stopping irrigation");

    _status.irrigating = false;
    _status.manualMode = false;
    _status.lastIrrigationTime = _currentTime;
    _currentDurationMinutes = 0;
    _status.currentDuration = 0;

    activateValve(false);
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
            DEBUG_PRINTF("IrrigationController: Schedule %d triggered\n", i);
            startIrrigation(_schedules[i].durationMinutes);
            break; // Only run one schedule at a time
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

void IrrigationController::activateValve(bool state) {
    digitalWrite(VALVE_PIN, state ? HIGH : LOW);
    DEBUG_PRINTF("IrrigationController: Valve %s\n", state ? "ON" : "OFF");
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

bool IrrigationController::addSchedule(uint8_t index, uint8_t hour, uint8_t minute,
                                       uint16_t durationMinutes, uint8_t weekdays) {
    if (index >= MAX_SCHEDULES) {
        return false;
    }

    if (hour > 23 || minute > 59) {
        return false;
    }

    if (durationMinutes < MIN_DURATION_MINUTES || durationMinutes > MAX_DURATION_MINUTES) {
        return false;
    }

    _schedules[index].enabled = true;
    _schedules[index].hour = hour;
    _schedules[index].minute = minute;
    _schedules[index].durationMinutes = durationMinutes;
    _schedules[index].weekdays = weekdays;

    DEBUG_PRINTF("IrrigationController: Schedule %d added: %02d:%02d, %d min\n",
                 index, hour, minute, durationMinutes);

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

    DynamicJsonDocument doc(1024);
    JsonArray array = doc.createNestedArray("schedules");

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        JsonObject schedule = array.createNestedObject();
        schedule["enabled"] = _schedules[i].enabled;
        schedule["hour"] = _schedules[i].hour;
        schedule["minute"] = _schedules[i].minute;
        schedule["duration"] = _schedules[i].durationMinutes;
        schedule["weekdays"] = _schedules[i].weekdays;
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

    DynamicJsonDocument doc(1024);
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
