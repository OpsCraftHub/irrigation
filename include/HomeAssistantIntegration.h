#ifndef HOME_ASSISTANT_INTEGRATION_H
#define HOME_ASSISTANT_INTEGRATION_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "IrrigationController.h"

class HomeAssistantIntegration {
public:
    HomeAssistantIntegration(IrrigationController* controller);
    ~HomeAssistantIntegration();

    // Initialization
    bool begin(const char* broker, uint16_t port,
               const char* user = nullptr, const char* password = nullptr);

    // Main update loop
    void update();

    // MQTT status
    bool isConnected() const { return _mqttClient != nullptr && _mqttClient->connected(); }

    // Publishing
    void publishState();
    void publishStatus();
    void publishSchedule();

    // Home Assistant Discovery
    void publishDiscovery();

private:
    // Internal methods
    void connectMQTT();
    void handleMQTTMessage(char* topic, byte* payload, unsigned int length);
    String buildTopic(const char* suffix);
    void subscribe();

    // Static callback wrapper
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
    static HomeAssistantIntegration* _instance;

    // Member variables
    IrrigationController* _controller;
    WiFiClient* _wifiClient;
    PubSubClient* _mqttClient;
    String _broker;
    uint16_t _port;
    String _user;
    String _password;
    unsigned long _lastReconnectAttempt;
    unsigned long _lastStatusUpdate;
};

#endif // HOME_ASSISTANT_INTEGRATION_H
