#include "WebAPIHandler.h"
#include "IrrigationController.h"
#include "HomeAssistantIntegration.h"
#include "NodeManager.h"
#include "WiFiManager.h"
extern Features features;
extern String nodeId;
extern String nodeRole;

WebAPIHandler::WebAPIHandler(WebServer* server,
                             IrrigationController* controller,
                             HomeAssistantIntegration* ha,
                             NodeManager* nm,
                             WiFiManager* wm)
    : _server(server)
    , _controller(controller)
    , _ha(ha)
    , _nm(nm)
    , _wm(wm)
{
}

void WebAPIHandler::begin() {
    // Schedule management APIs
    _server->on("/api/schedules", HTTP_GET, [this]() { handleGetSchedules(); });
    _server->on("/api/schedules", HTTP_POST, [this]() { handlePostSchedule(); });
    _server->on("/api/schedules", HTTP_DELETE, [this]() { handleDeleteSchedule(); });

    // Channel APIs
    _server->on("/api/channels/status", HTTP_GET, [this]() { handleGetChannelStatus(); });
    _server->on("/api/channel/invert", HTTP_POST, [this]() { handlePostChannelInvert(); });
    _server->on("/api/channel/enable", HTTP_POST, [this]() { handlePostChannelEnable(); });
    _server->on("/api/channels/available", HTTP_GET, [this]() { handleGetChannelsAvailable(); });

    // Schedule skip/unskip
    _server->on("/api/schedule/skip", HTTP_POST, [this]() { handlePostScheduleSkip(); });
    _server->on("/api/schedule/unskip", HTTP_POST, [this]() { handlePostScheduleUnskip(); });

    // Channel start/stop
    _server->on("/api/channel/start", HTTP_POST, [this]() { handlePostChannelStart(); });
    _server->on("/api/channel/stop", HTTP_POST, [this]() { handlePostChannelStop(); });

    // Node pairing API endpoints
    _server->on("/api/nodes/pending", HTTP_GET, [this]() { handleGetNodesPending(); });
    _server->on("/api/nodes/accept", HTTP_POST, [this]() { handlePostNodesAccept(); });
    _server->on("/api/nodes/reject", HTTP_POST, [this]() { handlePostNodesReject(); });
    _server->on("/api/nodes/rename", HTTP_POST, [this]() { handlePostNodesRename(); });
    _server->on("/api/nodes/unpair", HTTP_POST, [this]() { handlePostNodesUnpair(); });

    // MQTT configuration
    _server->on("/mqtt/save", HTTP_POST, [this]() { handlePostMqttSave(); });
    _server->on("/mqtt/test", HTTP_POST, [this]() { handlePostMqttTest(); });

    // WiFi/MQTT credential removal
    _server->on("/wifi/remove", HTTP_POST, [this]() { handlePostWifiRemove(); });
    _server->on("/mqtt/remove", HTTP_POST, [this]() { handlePostMqttRemove(); });

    // Feature flags config API
    _server->on("/api/config", HTTP_GET, [this]() { handleGetConfig(); });
    _server->on("/api/config", HTTP_POST, [this]() { handlePostConfig(); });

    // System restart
    _server->on("/system/restart", HTTP_POST, [this]() { handlePostSystemRestart(); });
}

// ================================================================
// Schedule management
// ================================================================

void WebAPIHandler::handleGetSchedules() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    DynamicJsonDocument doc(2048);
    doc["success"] = true;

    JsonArray channels = doc.createNestedArray("channels");
    // Only enabled local channels
    for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
        if (!_controller->isChannelEnabled(i + 1)) continue;
        JsonObject channel = channels.createNestedObject();
        channel["channel"] = i + 1;
        channel["pin"] = CHANNEL_PINS[i];
    }
    // Virtual channels for paired slaves
    if (_nm) {
        for (uint8_t i = 0; i < _nm->getSlaveCount(); i++) {
            const NodePeer* slave = _nm->getSlave(i);
            if (!slave) continue;
            for (uint8_t ch = 0; ch < slave->num_channels; ch++) {
                JsonObject channel = channels.createNestedObject();
                channel["channel"] = slave->base_virtual_ch + ch;
                channel["pin"] = 0;
                channel["remote"] = true;
                channel["slave"] = slave->name[0] ? slave->name : slave->node_id;
                channel["node_id"] = slave->node_id;
            }
        }
    }

    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count = 0;
    _controller->getSchedules(schedules, count);

    JsonArray array = doc.createNestedArray("schedules");
    for (uint8_t i = 0; i < count; i++) {
        if (!schedules[i].enabled) {
            continue;
        }

        JsonObject entry = array.createNestedObject();
        entry["id"] = i;
        entry["channel"] = schedules[i].channel;
        entry["hour"] = schedules[i].hour;
        entry["minute"] = schedules[i].minute;
        entry["duration"] = schedules[i].durationMinutes;
        entry["weekdays"] = schedules[i].weekdays;
        entry["pin"] = _controller->getChannelPin(schedules[i].channel);
        entry["skipped"] = _controller->isScheduleSkipped(i);
    }

    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void WebAPIHandler::handlePostSchedule() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    uint8_t channel = doc["channel"] | 0;
    uint8_t hour = doc["hour"] | 0;
    uint8_t minute = doc["minute"] | 0;
    uint16_t duration = doc["duration"] | DEFAULT_DURATION_MINUTES;
    uint8_t weekdays = doc["weekdays"] | 0x7F;
    int16_t editId = doc["id"] | -1;

    if (channel < 1 || channel > MAX_CHANNELS) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
        return;
    }
    if (hour > 23 || minute > 59) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid start time\"}");
        return;
    }
    if (duration < MIN_DURATION_MINUTES || duration > MAX_DURATION_MINUTES) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid duration\"}");
        return;
    }

    if (editId >= 0) {
        // Update existing schedule
        if (_controller->updateSchedule((uint8_t)editId, channel, hour, minute, duration, weekdays)) {
            // Sync to slave if virtual channel
            if (_nm && channel > NUM_LOCAL_CHANNELS) {
                const NodePeer* slave = _nm->getSlave(0);
                for (uint8_t s = 0; s < _nm->getSlaveCount(); s++) {
                    slave = _nm->getSlave(s);
                    if (slave && channel >= slave->base_virtual_ch &&
                        channel < slave->base_virtual_ch + slave->num_channels) {
                        _nm->sendScheduleSync(slave->node_id);
                        break;
                    }
                }
            }
            // Notify HA of schedule change
            if (_ha) {
                _ha->publishSchedule();
                _ha->publishStatus();
            }
            _server->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule updated\"}");
        } else {
            _server->send(500, "application/json", "{\"success\":false,\"message\":\"Unable to update schedule\"}");
        }
    } else {
        // Add new schedule
        int8_t index = _controller->addSchedule(channel, hour, minute, duration, weekdays);
        if (index >= 0) {
            // Sync to slave if virtual channel
            if (_nm && channel > NUM_LOCAL_CHANNELS) {
                for (uint8_t s = 0; s < _nm->getSlaveCount(); s++) {
                    const NodePeer* slave = _nm->getSlave(s);
                    if (slave && channel >= slave->base_virtual_ch &&
                        channel < slave->base_virtual_ch + slave->num_channels) {
                        _nm->sendScheduleSync(slave->node_id);
                        break;
                    }
                }
            }
            // Notify HA of schedule change
            if (_ha) {
                _ha->publishSchedule();
                _ha->publishStatus();
            }
            _server->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule saved\",\"index\":" + String(index) + "}");
        } else {
            _server->send(500, "application/json", "{\"success\":false,\"message\":\"Unable to save schedule\"}");
        }
    }
}

void WebAPIHandler::handleDeleteSchedule() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    if (!_server->hasArg("id")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing schedule id\"}");
        return;
    }

    uint8_t index = _server->arg("id").toInt();

    // Before removing, check if it belongs to a slave so we can sync after
    IrrigationSchedule sched = _controller->getSchedule(index);
    uint8_t schedCh = sched.channel;

    if (_controller->removeSchedule(index)) {
        // Sync to slave if the removed schedule was for a virtual channel
        if (_nm && schedCh > NUM_LOCAL_CHANNELS) {
            for (uint8_t s = 0; s < _nm->getSlaveCount(); s++) {
                const NodePeer* slave = _nm->getSlave(s);
                if (slave && schedCh >= slave->base_virtual_ch &&
                    schedCh < slave->base_virtual_ch + slave->num_channels) {
                    _nm->sendScheduleSync(slave->node_id);
                    break;
                }
            }
        }
        // Notify HA of schedule change
        if (_ha) {
            _ha->publishSchedule();
            _ha->publishStatus();
        }
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule removed\"}");
    } else {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove schedule\"}");
    }
}

// ================================================================
// Channel status and control
// ================================================================

void WebAPIHandler::handleGetChannelStatus() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    DynamicJsonDocument doc(512);
    doc["success"] = true;

    JsonArray channels = doc.createNestedArray("channels");
    // Only enabled local channels
    for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
        if (!_controller->isChannelEnabled(i + 1)) continue;
        JsonObject ch = channels.createNestedObject();
        ch["channel"] = i + 1;
        ch["pin"] = CHANNEL_PINS[i];
        ch["running"] = _controller->isChannelIrrigating(i + 1);
        ch["inverted"] = _controller->isChannelInverted(i + 1);
    }
    // Virtual channels for paired slaves
    if (_nm) {
        for (uint8_t s = 0; s < _nm->getSlaveCount(); s++) {
            const NodePeer* slave = _nm->getSlave(s);
            if (!slave) continue;
            for (uint8_t c = 0; c < slave->num_channels; c++) {
                uint8_t vch = slave->base_virtual_ch + c;
                JsonObject ch = channels.createNestedObject();
                ch["channel"] = vch;
                ch["pin"] = 0;
                ch["running"] = _controller->isChannelIrrigating(vch);
                ch["inverted"] = _controller->isChannelInverted(vch);
                ch["remote"] = true;
                ch["slave"] = slave->name[0] ? slave->name : slave->node_id;
            }
        }
    }

    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void WebAPIHandler::handlePostChannelInvert() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    uint8_t channel = doc["channel"] | 0;
    bool inverted = doc["inverted"] | false;

    if (channel < 1 || channel > MAX_CHANNELS) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
        return;
    }

    _controller->setChannelInverted(channel, inverted);
    DEBUG_PRINTF("WebAPIHandler: Channel %d invert set to %d\n", channel, inverted);
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Invert setting updated\"}");
}

void WebAPIHandler::handlePostChannelEnable() {
    if (!_controller || !_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Bad request\"}");
        return;
    }

    StaticJsonDocument<128> doc;
    deserializeJson(doc, _server->arg("plain"));
    uint8_t channel = doc["channel"] | 0;
    bool enabled = doc["enabled"] | true;

    if (channel < 1 || channel > NUM_LOCAL_CHANNELS) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
        return;
    }

    _controller->setChannelEnabled(channel, enabled);
    _server->send(200, "application/json", "{\"success\":true}");
}

void WebAPIHandler::handleGetChannelsAvailable() {
    StaticJsonDocument<512> doc;
    doc["success"] = true;
    JsonArray channels = doc.createNestedArray("channels");
    for (uint8_t i = 0; i < NUM_LOCAL_CHANNELS; i++) {
        JsonObject ch = channels.createNestedObject();
        ch["channel"] = i + 1;
        ch["pin"] = CHANNEL_PINS[i];
        ch["enabled"] = _controller->isChannelEnabled(i + 1);
    }
    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

// ================================================================
// Schedule skip/unskip
// ================================================================

void WebAPIHandler::handlePostScheduleSkip() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }
    DynamicJsonDocument doc(128);
    deserializeJson(doc, _server->arg("plain"));
    uint8_t id = doc["id"] | 255;
    if (id >= MAX_SCHEDULES) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid schedule id\"}");
        return;
    }
    _controller->skipSchedule(id);

    // Forward skip to slave if schedule is for a virtual channel
    if (_nm) {
        IrrigationSchedule sched = _controller->getSchedule(id);
        if (sched.channel > NUM_LOCAL_CHANNELS) {
            _nm->sendSkipToSlave(sched.channel, id);
        }
    }

    // Notify HA of schedule change
    if (_ha) {
        _ha->publishSchedule();
        _ha->publishStatus();
    }

    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule skipped\"}");
}

void WebAPIHandler::handlePostScheduleUnskip() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }
    DynamicJsonDocument doc(128);
    deserializeJson(doc, _server->arg("plain"));
    uint8_t id = doc["id"] | 255;
    if (id >= MAX_SCHEDULES) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid schedule id\"}");
        return;
    }
    _controller->unskipSchedule(id);

    // Forward unskip to slave if schedule is for a virtual channel
    if (_nm) {
        IrrigationSchedule sched = _controller->getSchedule(id);
        if (sched.channel > NUM_LOCAL_CHANNELS) {
            _nm->sendUnskipToSlave(sched.channel, id);
        }
    }

    // Notify HA of schedule change
    if (_ha) {
        _ha->publishSchedule();
        _ha->publishStatus();
    }

    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Schedule unskipped\"}");
}

// ================================================================
// Channel start/stop
// ================================================================

void WebAPIHandler::handlePostChannelStart() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    uint8_t channel = doc["channel"] | 0;
    uint16_t duration = doc["duration"] | DEFAULT_DURATION_MINUTES;

    if (channel < 1 || channel > MAX_CHANNELS) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
        return;
    }

    _controller->startIrrigation(channel, duration);
    DEBUG_PRINTF("WebAPIHandler: Manual start channel %d for %d min\n", channel, duration);
    // Notify HA of state change
    if (_ha) {
        _ha->publishChannelStates();
        _ha->publishIndividualStatus();
        _ha->publishState();
    }
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Channel started\"}");
}

void WebAPIHandler::handlePostChannelStop() {
    if (!_controller) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Controller not ready\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    uint8_t channel = doc["channel"] | 0;

    if (channel < 1 || channel > MAX_CHANNELS) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid channel\"}");
        return;
    }

    _controller->stopIrrigation(channel);
    DEBUG_PRINTF("WebAPIHandler: Manual stop channel %d\n", channel);
    // Notify HA of state change
    if (_ha) {
        _ha->publishChannelStates();
        _ha->publishIndividualStatus();
        _ha->publishState();
    }
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Channel stopped\"}");
}

// ================================================================
// Node pairing API endpoints
// ================================================================

void WebAPIHandler::handleGetNodesPending() {
    DynamicJsonDocument doc(1024);
    doc["success"] = true;

    if (_nm) {
        // Pending pair request
        if (_nm->hasPendingPair()) {
            const PendingPairRequest& req = _nm->getPendingPair();
            JsonObject pending = doc.createNestedObject("pending");
            pending["node_id"] = req.node_id;
            pending["name"] = req.name;
            pending["num_channels"] = req.num_channels;
            pending["ip"] = req.ip.toString();
        }

        // Paired slaves
        JsonArray slaves = doc.createNestedArray("slaves");
        for (uint8_t i = 0; i < _nm->getSlaveCount(); i++) {
            const NodePeer* peer = _nm->getSlave(i);
            if (!peer) continue;
            JsonObject s = slaves.createNestedObject();
            s["node_id"] = peer->node_id;
            s["name"] = peer->name;
            s["virtual_channel"] = peer->base_virtual_ch;
            s["num_channels"] = peer->num_channels;
            s["online"] = peer->online;
            s["rssi"] = peer->rssi;
        }
    }

    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void WebAPIHandler::handlePostNodesAccept() {
    if (!_nm) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
        return;
    }
    if (!_nm->hasPendingPair()) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"No pending pair request\"}");
        return;
    }
    _nm->acceptPendingPair();
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Pair accepted\"}");
}

void WebAPIHandler::handlePostNodesReject() {
    if (!_nm) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
        return;
    }
    if (!_nm->hasPendingPair()) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"No pending pair request\"}");
        return;
    }
    _nm->rejectPendingPair(PAIR_REJECT_USER);
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Pair rejected\"}");
}

void WebAPIHandler::handlePostNodesRename() {
    if (!_nm) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    StaticJsonDocument<256> reqDoc;
    DeserializationError error = deserializeJson(reqDoc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    const char* reqNodeId = reqDoc["node_id"] | "";
    const char* newName = reqDoc["name"] | "";
    if (strlen(reqNodeId) == 0 || strlen(newName) == 0) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"node_id and name required\"}");
        return;
    }

    if (_nm->renameSlave(reqNodeId, newName)) {
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"Slave renamed\"}");
    } else {
        _server->send(404, "application/json", "{\"success\":false,\"message\":\"Slave not found\"}");
    }
}

void WebAPIHandler::handlePostNodesUnpair() {
    if (!_nm) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"NodeManager not available\"}");
        return;
    }

    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing payload\"}");
        return;
    }

    StaticJsonDocument<128> reqDoc;
    DeserializationError error = deserializeJson(reqDoc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    const char* reqNodeId = reqDoc["node_id"] | "";
    if (strlen(reqNodeId) == 0) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"node_id required\"}");
        return;
    }

    if (_nm->unpairSlave(reqNodeId)) {
        // Refresh HA discovery to remove stale channel entities
        if (_ha) _ha->refreshDiscovery();
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"Slave unpaired\"}");
    } else {
        _server->send(404, "application/json", "{\"success\":false,\"message\":\"Slave not found\"}");
    }
}

// ================================================================
// MQTT configuration
// ================================================================

void WebAPIHandler::handlePostMqttSave() {
    String broker = _server->hasArg("broker") ? _server->arg("broker") : "";
    uint16_t port = _server->hasArg("port") ? _server->arg("port").toInt() : 1883;
    String user = _server->hasArg("user") ? _server->arg("user") : "";
    String password = _server->hasArg("password") ? _server->arg("password") : "";

    if (broker.length() == 0) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
        return;
    }

    if (!HomeAssistantIntegration::saveCredentials(broker, port, user, password)) {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to save credentials\"}");
        return;
    }

    // Enable mqtt feature flag and persist to config.json so it survives reboot
    features.mqtt = true;

    StaticJsonDocument<1024> cfgDoc;
    File cfgRead = LittleFS.open(CONFIG_FILE, "r");
    if (cfgRead) {
        deserializeJson(cfgDoc, cfgRead);
        cfgRead.close();
    }
    cfgDoc["node_id"] = nodeId;
    cfgDoc["role"] = nodeRole;
    JsonObject feat = cfgDoc.containsKey("features") ? cfgDoc["features"] : cfgDoc.createNestedObject("features");
    feat["mqtt"] = true;

    File cfgWrite = LittleFS.open(CONFIG_FILE, "w");
    if (cfgWrite) {
        serializeJson(cfgDoc, cfgWrite);
        cfgWrite.close();
    }

    DEBUG_PRINTLN("WebAPIHandler: MQTT credentials saved, mqtt feature enabled");
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Saved. Restarting...\"}");
    delay(2000);
    ESP.restart();
}

void WebAPIHandler::handlePostMqttTest() {
    String broker = _server->hasArg("broker") ? _server->arg("broker") : "";
    uint16_t port = _server->hasArg("port") ? _server->arg("port").toInt() : 1883;
    String user = _server->hasArg("user") ? _server->arg("user") : "";
    String password = _server->hasArg("password") ? _server->arg("password") : "";

    if (broker.length() == 0) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Broker required\"}");
        return;
    }

    bool success = HomeAssistantIntegration::testConnection(broker, port, user, password);

    if (success) {
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"Connection successful!\"}");
    } else {
        _server->send(200, "application/json", "{\"success\":false,\"message\":\"Connection failed\"}");
    }
}

// ================================================================
// WiFi/MQTT credential removal
// ================================================================

void WebAPIHandler::handlePostWifiRemove() {
    DEBUG_PRINTLN("WebAPIHandler: WiFi credentials removal requested via web interface");

    if (_wm && _wm->clearCredentials()) {
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials removed. Restarting...\"}");
        delay(2000);
        ESP.restart();
    } else {
        _server->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove WiFi credentials\"}");
    }
}

void WebAPIHandler::handlePostMqttRemove() {
    DEBUG_PRINTLN("WebAPIHandler: MQTT credentials removal requested via web interface");

    // Remove MQTT credentials file
    if (LittleFS.exists(MQTT_CREDENTIALS_FILE)) {
        if (LittleFS.remove(MQTT_CREDENTIALS_FILE)) {
            DEBUG_PRINTLN("WebAPIHandler: MQTT credentials removed successfully");

            // Disable mqtt feature flag in config.json
            features.mqtt = false;
            StaticJsonDocument<1024> cfgDoc;
            File cfgRead = LittleFS.open(CONFIG_FILE, "r");
            if (cfgRead) {
                deserializeJson(cfgDoc, cfgRead);
                cfgRead.close();
            }
            cfgDoc["features"]["mqtt"] = false;
            File cfgWrite = LittleFS.open(CONFIG_FILE, "w");
            if (cfgWrite) {
                serializeJson(cfgDoc, cfgWrite);
                cfgWrite.close();
            }

            _server->send(200, "application/json", "{\"success\":true,\"message\":\"MQTT credentials removed successfully\"}");
        } else {
            DEBUG_PRINTLN("WebAPIHandler: Failed to remove MQTT credentials file");
            _server->send(500, "application/json", "{\"success\":false,\"message\":\"Failed to remove MQTT credentials\"}");
        }
    } else {
        _server->send(200, "application/json", "{\"success\":true,\"message\":\"No MQTT credentials to remove\"}");
    }
}

// ================================================================
// Feature flags config API
// ================================================================

void WebAPIHandler::handleGetConfig() {
    StaticJsonDocument<512> doc;
    doc["success"] = true;
    doc["node_id"] = nodeId;
    doc["role"] = nodeRole;

    JsonObject feat = doc.createNestedObject("features");
    feat["multi_node"] = features.multi_node;
    feat["mqtt"] = features.mqtt;
    feat["web_ui"] = features.web_ui;
    feat["sensors"] = features.sensors;
    feat["battery"] = features.battery;
    feat["ota"] = features.ota;
    feat["debug"] = features.debug;

    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void WebAPIHandler::handlePostConfig() {
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"No body\"}");
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, _server->arg("plain"));
    if (error) {
        _server->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    // Update feature flags (only update fields that are present)
    JsonObject feat = doc["features"];
    if (!feat.isNull()) {
        if (feat.containsKey("multi_node")) features.multi_node = feat["multi_node"];
        if (feat.containsKey("mqtt")) features.mqtt = feat["mqtt"];
        if (feat.containsKey("web_ui")) features.web_ui = feat["web_ui"];
        if (feat.containsKey("sensors")) features.sensors = feat["sensors"];
        if (feat.containsKey("battery")) features.battery = feat["battery"];
        if (feat.containsKey("ota")) features.ota = feat["ota"];
        if (feat.containsKey("debug")) features.debug = feat["debug"];
    }

    // Update node identity if provided
    if (doc.containsKey("node_id")) nodeId = doc["node_id"].as<String>();
    if (doc.containsKey("role")) nodeRole = doc["role"].as<String>();

    // Save to LittleFS
    StaticJsonDocument<1024> saveDoc;
    saveDoc["node_id"] = nodeId;
    saveDoc["role"] = nodeRole;
    JsonObject saveFeat = saveDoc.createNestedObject("features");
    saveFeat["multi_node"] = features.multi_node;
    saveFeat["mqtt"] = features.mqtt;
    saveFeat["web_ui"] = features.web_ui;
    saveFeat["sensors"] = features.sensors;
    saveFeat["battery"] = features.battery;
    saveFeat["ota"] = features.ota;
    saveFeat["debug"] = features.debug;

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (file) {
        serializeJson(saveDoc, file);
        file.close();
        DEBUG_PRINTLN("WebAPIHandler: Config saved to LittleFS");
    }

    _server->send(200, "application/json",
        "{\"success\":true,\"message\":\"Config saved. Restart to apply changes.\"}");
}

// ================================================================
// System restart
// ================================================================

void WebAPIHandler::handlePostSystemRestart() {
    DEBUG_PRINTLN("WebAPIHandler: Restart requested via web interface");
    _server->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting device...\"}");
    delay(1000);
    ESP.restart();
}
