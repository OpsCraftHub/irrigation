#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"

class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();

    // Initialization
    bool begin(const char* ssid = nullptr, const char* password = nullptr);

    // Set controller reference for status page
    void setController(class IrrigationController* controller) { _controller = controller; }

    // Main update loop
    void update();

    // WiFi status
    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
    bool isConfigMode() const { return _configMode; }
    String getIPAddress() const { return WiFi.localIP().toString(); }
    int getRSSI() const { return WiFi.RSSI(); }

    // Configuration Portal
    void startConfigPortal(const char* apName = "ESP32-Setup");
    void stopConfigPortal();
    String scanNetworks();

    // Web status server (when connected)
    void startWebServer();
    void stopWebServer();

    // Time synchronization
    time_t getCurrentTime();
    bool isTimeSynced() const { return _timeSynced; }

    // OTA Updates
    void checkForUpdates();
    void performOTA();

    // Callbacks
    typedef void (*TimeUpdateCallback)(time_t);
    void setTimeUpdateCallback(TimeUpdateCallback callback) {
        _timeUpdateCallback = callback;
    }

private:
    // Internal methods
    void connectWiFi();
    void setupOTA();
    void syncTime();
    bool checkGitHubVersion(String& latestVersion);
    bool downloadFirmware(const String& url);
    void handleOTAProgress(unsigned int progress, unsigned int total);
    void handleOTAError(ota_error_t error);

    // Credential management
    bool loadCredentials();
    bool saveCredentials(const String& ssid, const String& password);

    // Web server setup for config portal
    void setupWebServer();
    void setupDNS();
    String getConfigPage();
    String getStatusPage();

    // Member variables
    String _ssid;
    String _password;
    bool _timeSynced;
    bool _configMode;
    unsigned long _lastReconnectAttempt;
    unsigned long _lastTimeSync;
    unsigned long _lastUpdateCheck;
    int _reconnectRetries;

    WiFiUDP* _ntpUDP;
    NTPClient* _ntpClient;
    TimeUpdateCallback _timeUpdateCallback;

    // Config portal
    WebServer* _webServer;
    DNSServer* _dnsServer;
    String _apName;

    // Controller reference for status page
    class IrrigationController* _controller;
};

#endif // WIFI_MANAGER_H
