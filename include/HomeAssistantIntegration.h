#ifndef HOME_ASSISTANT_INTEGRATION_H
#define HOME_ASSISTANT_INTEGRATION_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"
#include "IrrigationController.h"

class NodeManager;

class HomeAssistantIntegration {
public:
    HomeAssistantIntegration(IrrigationController* controller,
                             NodeManager* nodeManager = nullptr);
    ~HomeAssistantIntegration();

    // Initialization
    bool begin(const char* broker = nullptr, uint16_t port = 1883,
               const char* user = nullptr, const char* password = nullptr);

    // MQTT credentials management
    bool loadCredentials();
    static bool saveCredentials(const String& broker, uint16_t port, const String& user, const String& password);
    static bool testConnection(const String& broker, uint16_t port, const String& user, const String& password);

    // Get current config
    String getMqttBroker() const { return _broker; }
    uint16_t getMqttPort() const { return _port; }
    String getMqttUser() const { return _user; }

    // Main update loop
    void update();

    // MQTT status
    bool isConnected() const { return _mqttClient != nullptr && _mqttClient->connected(); }

    // Publishing
    void publishState();
    void publishStatus();
    void publishSchedule();
    void publishChannelStates();
    void publishIndividualStatus();

    // Home Assistant Discovery
    void publishDiscovery();
    void refreshDiscovery();

    // NodeManager integration (for slave forwarding)
    void setNodeManager(NodeManager* nm) { _nodeManager = nm; }

    // System state
    bool isSystemEnabled() const { return _systemEnabled; }

private:
    // Internal methods
    void connectMQTT();
    void handleMQTTMessage(char* topic, byte* payload, unsigned int length);
    String buildTopic(const char* suffix);
    void subscribe();

    // Discovery helpers
    void addDeviceBlock(JsonDocument& doc);
    void publishChannelSwitchDiscovery(uint8_t channel);
    void publishChannelDurationDiscovery(uint8_t channel);
    void publishChannelRuntimeDiscovery(uint8_t channel);
    void publishGlobalSensorDiscovery();
    void publishModeSelectDiscovery();
    void removeStaleDiscovery();

    // Message handlers
    void handleChannelCommand(uint8_t channel, const String& message);
    void handleChannelDurationSet(uint8_t channel, const String& message);
    void handleModeSet(const String& message);

    // Utility
    uint8_t parseDaysArray(JsonArray days);
    String toISO8601(time_t t);
    String weekdaysToDaysArray(uint8_t weekdays);
    void publishModeState();
    unsigned long getChannelTimeRemaining(uint8_t channel);
    bool isChannelActive(uint8_t channel);

    // Static callback wrapper
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
    static HomeAssistantIntegration* _instance;

    // Member variables
    IrrigationController* _controller;
    NodeManager* _nodeManager;
    WiFiClient* _wifiClient;
    PubSubClient* _mqttClient;
    String _broker;
    uint16_t _port;
    String _user;
    String _password;
    unsigned long _lastReconnectAttempt;
    unsigned long _lastStatusUpdate;

    // Per-channel state
    uint16_t _channelDuration[MAX_CHANNELS];
    bool _discoveredChannels[MAX_CHANNELS];
    uint8_t _lastDiscoveredCount;

    // System/mode state
    bool _systemEnabled;
    String _currentMode;  // "auto", "manual", "disabled"

    // Change detection
    bool _lastIrrigatingState;
    unsigned long _lastFastStatusUpdate;

    // Discovery management
    bool _needsDiscoveryPublish;
    String _lastDiscoveryVersion;

    // Reconciliation
    bool _retryPending[MAX_CHANNELS];
    unsigned long _commandSentTime[MAX_CHANNELS];
};

#endif // HOME_ASSISTANT_INTEGRATION_H
