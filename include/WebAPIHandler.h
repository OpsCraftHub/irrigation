#ifndef WEB_API_HANDLER_H
#define WEB_API_HANDLER_H

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"

// Forward declarations
class IrrigationController;
class HomeAssistantIntegration;
class NodeManager;
class WiFiManager;

class WebAPIHandler {
public:
    WebAPIHandler(WebServer* server,
                  IrrigationController* controller,
                  HomeAssistantIntegration* ha = nullptr,
                  NodeManager* nm = nullptr,
                  WiFiManager* wm = nullptr);

    void begin();  // Registers all /api/* routes on the server
    void setNodeManager(NodeManager* nm) { _nm = nm; }

private:
    WebServer* _server;
    IrrigationController* _controller;
    HomeAssistantIntegration* _ha;
    NodeManager* _nm;
    WiFiManager* _wm;

    // Route handlers
    void handleGetSchedules();
    void handlePostSchedule();
    void handleDeleteSchedule();
    void handleGetChannelStatus();
    void handlePostChannelInvert();
    void handlePostChannelEnable();
    void handleGetChannelsAvailable();
    void handlePostScheduleSkip();
    void handlePostScheduleUnskip();
    void handlePostChannelStart();
    void handlePostChannelStop();
    void handleGetNodesPending();
    void handlePostNodesAccept();
    void handlePostNodesReject();
    void handlePostNodesRename();
    void handlePostNodesUnpair();
    void handlePostMqttSave();
    void handlePostMqttTest();
    void handlePostWifiRemove();
    void handlePostMqttRemove();
    void handleGetConfig();
    void handlePostConfig();
    void handlePostSystemRestart();
};

#endif // WEB_API_HANDLER_H
