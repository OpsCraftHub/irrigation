#include "NodeManager.h"
#include "IrrigationController.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

NodeManager::NodeManager()
    : _role(NODE_ROLE_MASTER),
      _controller(nullptr),
      _seq(0),
      _slaveCount(0),
      _masterPort(NODE_UDP_PORT),
      _masterFound(false),
      _lastMdnsQuery(0),
      _mdnsStarted(false),
      _dedupIdx(0),
      _lastHeartbeat(0),
      _lastStatusSend(0),
      _initialized(false),
      _pairRequestCallback(nullptr),
      _paired(false),
      _assignedVirtualCh(0),
      _lastPairAttempt(0) {
    memset(_nodeId, 0, sizeof(_nodeId));
    strncpy(_nodeId, DEFAULT_NODE_ID, sizeof(_nodeId) - 1);
    memset(_slaves, 0, sizeof(_slaves));
    memset(_masterNodeId, 0, sizeof(_masterNodeId));
    memset(_outbox, 0, sizeof(_outbox));
    memset(_dedup, 0, sizeof(_dedup));
    memset(&_pendingPair, 0, sizeof(_pendingPair));
    memset(_nodeName, 0, sizeof(_nodeName));
    strncpy(_nodeName, "Slave", sizeof(_nodeName) - 1);
}

NodeManager::~NodeManager() {
    if (_initialized) {
        _udp.stop();
    }
}

void NodeManager::setNodeId(const char* id) {
    strncpy(_nodeId, id, sizeof(_nodeId) - 1);
    _nodeId[sizeof(_nodeId) - 1] = '\0';
}

void NodeManager::setNodeName(const char* name) {
    strncpy(_nodeName, name, sizeof(_nodeName) - 1);
    _nodeName[sizeof(_nodeName) - 1] = '\0';
}

bool NodeManager::begin() {
    DEBUG_PRINTLN("NodeManager: Initializing UDP transport...");

    if (!_udp.begin(NODE_UDP_PORT)) {
        DEBUG_PRINTLN("NodeManager: ERROR - Failed to start UDP listener");
        return false;
    }

    _initialized = true;

    // Load pairing state from SPIFFS
    if (_role == NODE_ROLE_MASTER) {
        loadPairedSlaves();
    } else {
        loadPairedMaster();
    }

    const char* roleStr = (_role == NODE_ROLE_MASTER) ? "MASTER" : "SLAVE";
    DEBUG_PRINTF("NodeManager: UDP listening on port %d, role=%s, node_id=%s\n",
                 NODE_UDP_PORT, roleStr, _nodeId);

    if (_role == NODE_ROLE_SLAVE) {
        DEBUG_PRINTF("NodeManager: paired=%d, name=%s\n", _paired, _nodeName);
    }

    return true;
}

void NodeManager::update() {
    if (!_initialized) return;

    // Lazy mDNS setup once WiFi is connected
    if (!_mdnsStarted && WiFi.isConnected()) {
        MDNS.begin(_nodeId);
        if (_role == NODE_ROLE_MASTER) {
            advertiseMdns();
        }
        _mdnsStarted = true;
        DEBUG_PRINTF("NodeManager: mDNS started, hostname=%s.local\n", _nodeId);
    }

    // Receive incoming UDP packets
    receiveUdp();

    // Process retry outbox
    processOutbox();

    unsigned long now = millis();

    if (_role == NODE_ROLE_MASTER) {
        // Master: send heartbeat to all slaves periodically
        if (now - _lastHeartbeat >= NODE_HEARTBEAT_INTERVAL) {
            _lastHeartbeat = now;
            sendHeartbeat();
            checkPeerTimeouts();
        }
        // Master: auto-reject stale pending pair requests
        checkPairTimeout();
    } else {
        // Slave: discover master if not found yet
        if (!_masterFound && _mdnsStarted) {
            if (now - _lastMdnsQuery >= NODE_MDNS_RETRY_INTERVAL) {
                _lastMdnsQuery = now;
                _masterFound = discoverMaster();
            }
        }

        // Slave: if master found but not yet paired, send pair request periodically
        if (_masterFound && !_paired) {
            if (now - _lastPairAttempt >= NODE_PAIR_RETRY_INTERVAL) {
                _lastPairAttempt = now;
                sendPairRequest();
            }
        }

        // Slave: send heartbeat + status when paired
        if (_masterFound && _paired) {
            if (now - _lastHeartbeat >= NODE_HEARTBEAT_INTERVAL) {
                _lastHeartbeat = now;
                sendHeartbeat();
            }
            if (now - _lastStatusSend >= NODE_STATUS_INTERVAL) {
                _lastStatusSend = now;
                sendStatus();
            }
        }
    }
}

// ============================================================================
// mDNS
// ============================================================================

void NodeManager::advertiseMdns() {
    MDNS.addService("irrigation", "udp", NODE_UDP_PORT);
    MDNS.addServiceTxt("irrigation", "udp", "role", "master");
    MDNS.addServiceTxt("irrigation", "udp", "id", String(_nodeId));
    DEBUG_PRINTF("NodeManager: mDNS service registered (_irrigation._udp port %d)\n",
                 NODE_UDP_PORT);
}

bool NodeManager::discoverMaster() {
    DEBUG_PRINTLN("NodeManager: Querying mDNS for _irrigation._udp...");
    int n = MDNS.queryService("irrigation", "udp");
    if (n <= 0) {
        DEBUG_PRINTLN("NodeManager: No master found via mDNS");
        return false;
    }

    for (int i = 0; i < n; i++) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        _masterIp = MDNS.address(i);
#else
        _masterIp = MDNS.IP(i);
#endif
        _masterPort = MDNS.port(i);

        // Try to extract master's node_id from mDNS TXT record
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        String id = MDNS.txt(i, "id");
#else
        String id = "";
        for (int t = 0; t < MDNS.numTxt(i); t++) {
            if (String(MDNS.txtKey(i, t)) == "id") {
                id = MDNS.txt(i, t);
                break;
            }
        }
#endif
        if (id.length() > 0 && _masterNodeId[0] == '\0') {
            strncpy(_masterNodeId, id.c_str(), sizeof(_masterNodeId) - 1);
            _masterNodeId[sizeof(_masterNodeId) - 1] = '\0';
            DEBUG_PRINTF("NodeManager: Master node_id from mDNS: '%s'\n", _masterNodeId);
        }

        DEBUG_PRINTF("NodeManager: Found master at %s:%d\n",
                     _masterIp.toString().c_str(), _masterPort);
        return true;
    }
    return false;
}

// ============================================================================
// UDP Transport
// ============================================================================

bool NodeManager::sendUdp(IPAddress ip, uint16_t port, const IrrigationMsg& msg) {
    if (!WiFi.isConnected()) return false;
    _udp.beginPacket(ip, port);
    _udp.write((const uint8_t*)&msg, sizeof(IrrigationMsg));
    return _udp.endPacket() == 1;
}

void NodeManager::receiveUdp() {
    int packetSize = _udp.parsePacket();
    while (packetSize > 0) {
        uint8_t buf[sizeof(IrrigationMsg)];
        int len = _udp.read(buf, sizeof(buf));
        IPAddress senderIp = _udp.remoteIP();
        uint16_t senderPort = _udp.remotePort();

        handleMessage(senderIp, senderPort, buf, len);

        packetSize = _udp.parsePacket();
    }
}

// ============================================================================
// Reliability Layer
// ============================================================================

void NodeManager::enqueueOutbox(const IrrigationMsg& msg, IPAddress ip, uint16_t port) {
    for (uint8_t i = 0; i < OUTBOX_SIZE; i++) {
        if (!_outbox[i].active) {
            _outbox[i].msg = msg;
            _outbox[i].dst_ip = ip;
            _outbox[i].dst_port = port;
            _outbox[i].last_send = millis();
            _outbox[i].retries = 0;
            _outbox[i].active = true;
            return;
        }
    }
    DEBUG_PRINTLN("NodeManager: Outbox full, dropping message");
}

void NodeManager::processOutbox() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < OUTBOX_SIZE; i++) {
        if (!_outbox[i].active) continue;

        // Exponential backoff: 200ms, 400ms, 800ms
        unsigned long interval = 200UL << _outbox[i].retries;
        if (now - _outbox[i].last_send < interval) continue;

        if (_outbox[i].retries >= NODE_MAX_RETRIES) {
            DEBUG_PRINTF("NodeManager: Message seq=%d dropped after %d retries\n",
                         _outbox[i].msg.seq, NODE_MAX_RETRIES);
            _outbox[i].active = false;
            continue;
        }

        _outbox[i].retries++;
        _outbox[i].last_send = now;
        sendUdp(_outbox[i].dst_ip, _outbox[i].dst_port, _outbox[i].msg);
        DEBUG_PRINTF("NodeManager: Retry %d for seq=%d\n",
                     _outbox[i].retries, _outbox[i].msg.seq);
    }
}

void NodeManager::removeFromOutbox(uint16_t seq) {
    for (uint8_t i = 0; i < OUTBOX_SIZE; i++) {
        if (_outbox[i].active && _outbox[i].msg.seq == seq) {
            _outbox[i].active = false;
            return;
        }
    }
}

bool NodeManager::isDuplicate(const char* srcId, uint16_t seq) {
    uint32_t hash = hashNodeId(srcId);
    unsigned long now = millis();
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) {
        if (_dedup[i].src_hash == hash && _dedup[i].seq == seq) {
            if (now - _dedup[i].timestamp < NODE_DEDUP_WINDOW) {
                return true;
            }
        }
    }
    return false;
}

void NodeManager::addDedup(const char* srcId, uint16_t seq) {
    _dedup[_dedupIdx].src_hash = hashNodeId(srcId);
    _dedup[_dedupIdx].seq = seq;
    _dedup[_dedupIdx].timestamp = millis();
    _dedupIdx = (_dedupIdx + 1) % DEDUP_SIZE;
}

// ============================================================================
// Master: register a slave
// ============================================================================

bool NodeManager::addSlave(const char* nodeId, uint8_t baseVirtualCh) {
    // Duplicate check — skip if already registered
    NodePeer* existing = findSlaveByNodeId(nodeId);
    if (existing) {
        DEBUG_PRINTF("NodeManager: Slave '%s' already registered at virtual_ch=%d\n",
                     nodeId, existing->base_virtual_ch);
        return true;
    }

    if (_slaveCount >= MAX_SLAVES) {
        DEBUG_PRINTLN("NodeManager: Max slaves reached");
        return false;
    }

    NodePeer& peer = _slaves[_slaveCount];
    strncpy(peer.node_id, nodeId, sizeof(peer.node_id) - 1);
    peer.node_id[sizeof(peer.node_id) - 1] = '\0';
    memset(peer.name, 0, sizeof(peer.name));
    peer.ip = IPAddress(0, 0, 0, 0);
    peer.port = NODE_UDP_PORT;
    peer.base_virtual_ch = baseVirtualCh;
    peer.num_channels = 1;  // Updated when heartbeat arrives
    peer.online = false;
    peer.last_seen = 0;
    peer.irrigating = false;
    peer.time_remaining = 0;
    peer.rssi = 0;
    _slaveCount++;

    DEBUG_PRINTF("NodeManager: Added slave '%s' virtual_ch=%d (IP resolved on first heartbeat)\n",
                 nodeId, baseVirtualCh);
    return true;
}

// ============================================================================
// Master: send commands to virtual channels
// ============================================================================

bool NodeManager::sendStart(uint8_t virtualChannel, uint16_t durationMinutes) {
    NodePeer* peer = findSlaveByVirtualCh(virtualChannel);
    if (!peer) {
        DEBUG_PRINTF("NodeManager: No slave for virtual channel %d\n", virtualChannel);
        return false;
    }
    if (!peer->online || peer->ip == IPAddress(0, 0, 0, 0)) {
        DEBUG_PRINTF("NodeManager: Slave '%s' offline, cannot start ch %d\n",
                     peer->node_id, virtualChannel);
        return false;
    }

    // Map virtual channel to slave's local channel
    uint8_t localCh = virtualChannel - peer->base_virtual_ch + 1;

    IrrigationMsg msg = {};
    fillHeader(msg, MSG_CMD_START, peer->node_id, localCh);
    msg.command.duration = durationMinutes;

    DEBUG_PRINTF("NodeManager: Sending CMD_START to '%s' local_ch=%d duration=%d\n",
                 peer->node_id, localCh, durationMinutes);

    bool sent = sendUdp(peer->ip, peer->port, msg);
    if (sent) {
        enqueueOutbox(msg, peer->ip, peer->port);
    }
    return sent;
}

bool NodeManager::sendStop(uint8_t virtualChannel) {
    NodePeer* peer = findSlaveByVirtualCh(virtualChannel);
    if (!peer) {
        DEBUG_PRINTF("NodeManager: No slave for virtual channel %d\n", virtualChannel);
        return false;
    }

    uint8_t localCh = virtualChannel - peer->base_virtual_ch + 1;

    IrrigationMsg msg = {};
    fillHeader(msg, MSG_CMD_STOP, peer->node_id, localCh);

    DEBUG_PRINTF("NodeManager: Sending CMD_STOP to '%s' local_ch=%d\n",
                 peer->node_id, localCh);

    bool sent = sendUdp(peer->ip, peer->port, msg);
    if (sent) {
        enqueueOutbox(msg, peer->ip, peer->port);
    }
    return sent;
}

// ============================================================================
// Message dispatcher
// ============================================================================

void NodeManager::handleMessage(IPAddress senderIp, uint16_t senderPort,
                                const uint8_t* data, int len) {
    if ((size_t)len < sizeof(IrrigationMsg) - 20) return;  // At least header

    const IrrigationMsg& msg = *reinterpret_cast<const IrrigationMsg*>(data);

    if (msg.version != NODE_PROTO_VERSION) {
        DEBUG_PRINTF("NodeManager: Ignoring msg with version %d (expected %d)\n",
                     msg.version, NODE_PROTO_VERSION);
        return;
    }

    // Check destination (accept broadcast or messages addressed to us)
    if (msg.dst_id[0] != '*' && strncmp(msg.dst_id, _nodeId, sizeof(msg.dst_id)) != 0) {
        return;  // Not for us
    }

    // Dedup (skip for ACK messages — they are responses)
    if (msg.type != MSG_CMD_ACK && msg.type != MSG_HEARTBEAT_ACK) {
        if (isDuplicate(msg.src_id, msg.seq)) {
            return;
        }
        addDedup(msg.src_id, msg.seq);
    }

    switch (msg.type) {
        case MSG_CMD_START:     handleCmdStart(senderIp, senderPort, msg); break;
        case MSG_CMD_STOP:      handleCmdStop(senderIp, senderPort, msg); break;
        case MSG_CMD_SKIP:      handleCmdSkip(senderIp, senderPort, msg); break;
        case MSG_CMD_ACK:       handleCmdAck(msg); break;
        case MSG_SCHEDULE_SET:  handleScheduleSet(senderIp, senderPort, msg); break;
        case MSG_SCHEDULE_ACK:  handleScheduleAck(msg); break;
        case MSG_STATUS:        handleStatus(senderIp, msg); break;
        case MSG_HEARTBEAT:     handleHeartbeat(senderIp, senderPort, msg); break;
        case MSG_HEARTBEAT_ACK: handleHeartbeatAck(msg); break;
        case MSG_PAIR_REQUEST:  handlePairRequest(senderIp, senderPort, msg); break;
        case MSG_PAIR_ACCEPT:   handlePairAccept(msg); break;
        case MSG_PAIR_REJECT:   handlePairReject(msg); break;
        default:
            DEBUG_PRINTF("NodeManager: Unknown msg type 0x%02X\n", msg.type);
            break;
    }
}

// ============================================================================
// Slave-side handlers (receives commands from master)
// ============================================================================

void NodeManager::handleCmdStart(IPAddress senderIp, uint16_t senderPort,
                                 const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;
    if (!_controller) return;

    uint8_t ch = msg.channel;
    uint16_t duration = msg.command.duration;
    DEBUG_PRINTF("NodeManager: Received CMD_START ch=%d duration=%d\n", ch, duration);

    _controller->startIrrigation(ch, duration);
    sendAck(senderIp, senderPort, MSG_CMD_START, ACK_OK, msg.seq);
}

void NodeManager::handleCmdStop(IPAddress senderIp, uint16_t senderPort,
                                const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;
    if (!_controller) return;

    uint8_t ch = msg.channel;
    DEBUG_PRINTF("NodeManager: Received CMD_STOP ch=%d\n", ch);

    _controller->stopIrrigation(ch);
    sendAck(senderIp, senderPort, MSG_CMD_STOP, ACK_OK, msg.seq);
}

// ============================================================================
// Master-side handlers (receives status/heartbeat from slaves)
// ============================================================================

void NodeManager::handleCmdAck(const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_MASTER) return;

    const char* typeStr = (msg.ack.acked_type == MSG_CMD_START) ? "START" :
                          (msg.ack.acked_type == MSG_CMD_STOP)  ? "STOP" :
                          (msg.ack.acked_type == MSG_CMD_SKIP)  ? "SKIP" :
                          (msg.ack.acked_type == MSG_SCHEDULE_SET) ? "SCHED_SET" : "?";
    DEBUG_PRINTF("NodeManager: ACK for %s (seq=%d), result=%d\n",
                 typeStr, msg.ack.acked_seq, msg.ack.result);

    removeFromOutbox(msg.ack.acked_seq);
}

void NodeManager::handleStatus(IPAddress senderIp, const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_MASTER) return;

    NodePeer* peer = findSlaveByNodeId(msg.src_id);
    if (!peer) return;

    peer->last_seen = millis();
    peer->irrigating = (msg.status.state == 1);
    peer->time_remaining = msg.status.time_remaining;
    peer->rssi = msg.status.rssi;

    // Update IP if changed (DHCP renewal)
    if (peer->ip != senderIp) {
        DEBUG_PRINTF("NodeManager: Slave '%s' IP updated to %s\n",
                     peer->node_id, senderIp.toString().c_str());
        peer->ip = senderIp;
    }

    // Update the virtual channel state on the controller
    if (_controller) {
        uint8_t virtualCh = peer->base_virtual_ch + msg.channel - 1;
        _controller->setRemoteChannelStatus(virtualCh, peer->irrigating,
                                            msg.status.time_remaining);
    }
}

void NodeManager::handleHeartbeat(IPAddress senderIp, uint16_t senderPort,
                                  const IrrigationMsg& msg) {
    if (_role == NODE_ROLE_MASTER) {
        // Master receives heartbeat from slave
        NodePeer* peer = findSlaveByNodeId(msg.src_id);
        if (!peer) {
            DEBUG_PRINTF("NodeManager: Heartbeat from unknown node '%s' at %s\n",
                         msg.src_id, senderIp.toString().c_str());
            return;
        }

        bool wasOffline = !peer->online;
        peer->online = true;
        peer->last_seen = millis();
        peer->num_channels = msg.heartbeat.num_channels;

        // Update IP if changed
        if (peer->ip != senderIp) {
            DEBUG_PRINTF("NodeManager: Slave '%s' IP updated to %s\n",
                         peer->node_id, senderIp.toString().c_str());
            peer->ip = senderIp;
            peer->port = senderPort;
        }

        if (wasOffline) {
            DEBUG_PRINTF("NodeManager: Slave '%s' ONLINE (uptime=%lus, channels=%d, IP=%s)\n",
                         peer->node_id, (unsigned long)msg.heartbeat.uptime,
                         msg.heartbeat.num_channels, senderIp.toString().c_str());

            // Push schedules to slave on reconnect
            syncSchedulesForSlave(peer);
        }

        // Reply with HEARTBEAT_ACK containing current epoch time
        IrrigationMsg ack = {};
        fillHeader(ack, MSG_HEARTBEAT_ACK, msg.src_id, 0);
        if (_controller && _controller->hasValidTime()) {
            ack.heartbeat_ack.epoch_time = (uint32_t)_controller->getCurrentTime();
        }
        sendUdp(senderIp, senderPort, ack);

    } else {
        // Slave receives heartbeat from master — update master info
        if (!_masterFound) {
            _masterIp = senderIp;
            _masterPort = senderPort;
            strncpy(_masterNodeId, msg.src_id, sizeof(_masterNodeId) - 1);
            _masterNodeId[sizeof(_masterNodeId) - 1] = '\0';
            _masterFound = true;
            DEBUG_PRINTF("NodeManager: Master found via heartbeat at %s\n",
                         senderIp.toString().c_str());
        }
    }
}

void NodeManager::handleHeartbeatAck(const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;

    // Slave receives time sync from master
    if (msg.heartbeat_ack.epoch_time > 0 && _controller) {
        _controller->setCurrentTime((time_t)msg.heartbeat_ack.epoch_time);
    }
}

// ============================================================================
// Sending helpers
// ============================================================================

void NodeManager::sendHeartbeat() {
    IrrigationMsg msg = {};
    fillHeader(msg, MSG_HEARTBEAT, NODE_BROADCAST_ID, 0);
    msg.heartbeat.uptime = millis() / 1000;
    msg.heartbeat.num_channels = NUM_LOCAL_CHANNELS;
    msg.heartbeat.role = _role;
    msg.heartbeat.pending_cmds = 0;

    if (_role == NODE_ROLE_MASTER) {
        // Send heartbeat to each known slave
        for (uint8_t i = 0; i < _slaveCount; i++) {
            if (_slaves[i].ip != IPAddress(0, 0, 0, 0)) {
                strncpy(msg.dst_id, _slaves[i].node_id, sizeof(msg.dst_id) - 1);
                msg.dst_id[sizeof(msg.dst_id) - 1] = '\0';
                msg.seq = _seq++;
                sendUdp(_slaves[i].ip, _slaves[i].port, msg);
            }
        }
    } else if (_masterFound) {
        strncpy(msg.dst_id, _masterNodeId, sizeof(msg.dst_id) - 1);
        msg.dst_id[sizeof(msg.dst_id) - 1] = '\0';
        sendUdp(_masterIp, _masterPort, msg);
    }
}

void NodeManager::sendStatus() {
    if (_role != NODE_ROLE_SLAVE) return;
    if (!_controller) return;
    if (!_masterFound) return;

    // Send status for each local channel
    for (uint8_t ch = 1; ch <= NUM_LOCAL_CHANNELS; ch++) {
        IrrigationMsg msg = {};
        fillHeader(msg, MSG_STATUS, _masterNodeId, ch);

        msg.status.state = _controller->isChannelIrrigating(ch) ? 1 : 0;

        // Calculate remaining seconds for this channel
        SystemStatus st = _controller->getStatus();
        uint8_t idx = ch - 1;
        if (st.channelIrrigating[idx] && st.channelStartTime[idx] > 0) {
            unsigned long elapsed = (millis() - st.channelStartTime[idx]) / 1000;
            unsigned long totalSec = (unsigned long)st.channelDuration[idx] * 60;
            msg.status.time_remaining = (elapsed < totalSec) ?
                (uint16_t)(totalSec - elapsed) : 0;
        } else {
            msg.status.time_remaining = 0;
        }

        msg.status.flow_litres = 0;
        msg.status.battery_pct = 0xFF;  // Mains powered
        msg.status.tank_pct = 0xFF;     // No sensor
        msg.status.rssi = (int8_t)WiFi.RSSI();

        sendUdp(_masterIp, _masterPort, msg);
    }
}

void NodeManager::sendAck(IPAddress ip, uint16_t port, uint8_t ackedType,
                          uint8_t result, uint16_t ackedSeq) {
    IrrigationMsg msg = {};
    fillHeader(msg, MSG_CMD_ACK, NODE_BROADCAST_ID, 0);
    msg.ack.acked_type = ackedType;
    msg.ack.result = result;
    msg.ack.acked_seq = ackedSeq;

    sendUdp(ip, port, msg);
}

// ============================================================================
// Peer lookup
// ============================================================================

NodePeer* NodeManager::findSlaveByNodeId(const char* nodeId) {
    for (uint8_t i = 0; i < _slaveCount; i++) {
        if (strncmp(_slaves[i].node_id, nodeId, sizeof(_slaves[i].node_id)) == 0) {
            return &_slaves[i];
        }
    }
    return nullptr;
}

NodePeer* NodeManager::findSlaveByVirtualCh(uint8_t virtualCh) {
    for (uint8_t i = 0; i < _slaveCount; i++) {
        uint8_t base = _slaves[i].base_virtual_ch;
        uint8_t count = _slaves[i].num_channels;
        if (count == 0) count = 1;  // Fallback before first heartbeat
        if (virtualCh >= base && virtualCh < base + count) {
            return &_slaves[i];
        }
    }
    return nullptr;
}

const NodePeer* NodeManager::getSlave(uint8_t index) const {
    if (index >= _slaveCount) return nullptr;
    return &_slaves[index];
}

// ============================================================================
// Timeout check
// ============================================================================

void NodeManager::checkPeerTimeouts() {
    unsigned long now = millis();

    for (uint8_t i = 0; i < _slaveCount; i++) {
        if (_slaves[i].online && _slaves[i].last_seen > 0) {
            if (now - _slaves[i].last_seen >= NODE_PEER_TIMEOUT) {
                DEBUG_PRINTF("NodeManager: Slave '%s' OFFLINE (timeout)\n",
                             _slaves[i].node_id);
                _slaves[i].online = false;
                _slaves[i].irrigating = false;
                _slaves[i].time_remaining = 0;

                // Mark virtual channels as not irrigating
                if (_controller) {
                    for (uint8_t ch = 0; ch < _slaves[i].num_channels; ch++) {
                        _controller->setRemoteChannelStatus(
                            _slaves[i].base_virtual_ch + ch, false, 0);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Schedule Sync: Master-side
// ============================================================================

void NodeManager::syncSchedulesForSlave(NodePeer* slave) {
    if (!slave || !_controller) return;
    if (!slave->online || slave->ip == IPAddress(0, 0, 0, 0)) {
        DEBUG_PRINTF("NodeManager: Slave '%s' offline, deferring schedule sync\n",
                     slave->node_id);
        return;
    }

    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count = 0;
    _controller->getSchedules(schedules, count);

    uint8_t baseVch = slave->base_virtual_ch;
    uint8_t numCh = slave->num_channels;
    if (numCh == 0) numCh = 1;

    // Collect matching schedules and assign slave-local indices
    uint8_t slaveIdx = 0;
    for (uint8_t i = 0; i < count && slaveIdx < MAX_SCHEDULES; i++) {
        if (!schedules[i].enabled) continue;
        uint8_t ch = schedules[i].channel;
        if (ch < baseVch || ch >= baseVch + numCh) continue;

        uint8_t localCh = ch - baseVch + 1;

        IrrigationMsg msg = {};
        fillHeader(msg, MSG_SCHEDULE_SET, slave->node_id, localCh);
        msg.schedule.index = slaveIdx;
        msg.schedule.enabled = 1;
        msg.schedule.hour = schedules[i].hour;
        msg.schedule.minute = schedules[i].minute;
        msg.schedule.duration = schedules[i].durationMinutes;
        msg.schedule.weekdays = schedules[i].weekdays;

        DEBUG_PRINTF("NodeManager: Syncing schedule idx=%d ch=%d->local=%d %02d:%02d %dmin to '%s'\n",
                     slaveIdx, ch, localCh, schedules[i].hour, schedules[i].minute,
                     schedules[i].durationMinutes, slave->node_id);

        sendUdp(slave->ip, slave->port, msg);
        enqueueOutbox(msg, slave->ip, slave->port);
        slaveIdx++;
    }

    // Clear remaining slave schedule slots (send enabled=false)
    for (uint8_t i = slaveIdx; i < MAX_SCHEDULES; i++) {
        IrrigationMsg msg = {};
        fillHeader(msg, MSG_SCHEDULE_SET, slave->node_id, 0);
        msg.schedule.index = i;
        msg.schedule.enabled = 0;

        sendUdp(slave->ip, slave->port, msg);
        // No outbox for clears — best-effort is fine
    }

    DEBUG_PRINTF("NodeManager: Schedule sync complete for '%s': %d schedules pushed\n",
                 slave->node_id, slaveIdx);
}

void NodeManager::sendScheduleSync(const char* slaveNodeId) {
    if (_role != NODE_ROLE_MASTER) return;

    NodePeer* slave = findSlaveByNodeId(slaveNodeId);
    if (!slave) {
        DEBUG_PRINTF("NodeManager: sendScheduleSync — slave '%s' not found\n", slaveNodeId);
        return;
    }
    syncSchedulesForSlave(slave);
}

void NodeManager::sendSkipToSlave(uint8_t virtualChannel, uint8_t scheduleIndex) {
    if (_role != NODE_ROLE_MASTER) return;

    NodePeer* slave = findSlaveByVirtualCh(virtualChannel);
    if (!slave) {
        DEBUG_PRINTF("NodeManager: sendSkipToSlave — no slave for virtual ch %d\n", virtualChannel);
        return;
    }
    if (!slave->online || slave->ip == IPAddress(0, 0, 0, 0)) {
        DEBUG_PRINTF("NodeManager: Slave '%s' offline, cannot send skip\n", slave->node_id);
        return;
    }

    // Remap master schedule index to slave-local index
    // Walk master schedules for this slave's channel range to find the local index
    IrrigationSchedule schedules[MAX_SCHEDULES];
    uint8_t count = 0;
    _controller->getSchedules(schedules, count);

    uint8_t baseVch = slave->base_virtual_ch;
    uint8_t numCh = slave->num_channels;
    if (numCh == 0) numCh = 1;

    uint8_t slaveLocalIdx = 0;
    bool found = false;
    for (uint8_t i = 0; i < count; i++) {
        if (!schedules[i].enabled) continue;
        uint8_t ch = schedules[i].channel;
        if (ch < baseVch || ch >= baseVch + numCh) continue;
        if (i == scheduleIndex) {
            found = true;
            break;
        }
        slaveLocalIdx++;
    }

    if (!found) {
        DEBUG_PRINTF("NodeManager: sendSkipToSlave — schedule %d not mapped to slave '%s'\n",
                     scheduleIndex, slave->node_id);
        return;
    }

    IrrigationMsg msg = {};
    fillHeader(msg, MSG_CMD_SKIP, slave->node_id, 0);
    msg.schedule.index = slaveLocalIdx;  // Reuse schedule payload for skip index

    DEBUG_PRINTF("NodeManager: Sending SKIP to '%s' slave_idx=%d (master_idx=%d)\n",
                 slave->node_id, slaveLocalIdx, scheduleIndex);

    sendUdp(slave->ip, slave->port, msg);
    enqueueOutbox(msg, slave->ip, slave->port);
}

// ============================================================================
// Schedule Sync: Slave-side handlers
// ============================================================================

void NodeManager::handleScheduleSet(IPAddress senderIp, uint16_t senderPort,
                                    const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;
    if (!_controller) return;

    uint8_t index = msg.schedule.index;
    uint8_t localCh = msg.channel;
    bool enabled = msg.schedule.enabled != 0;

    if (enabled) {
        uint8_t hour = msg.schedule.hour;
        uint8_t minute = msg.schedule.minute;
        uint16_t duration = msg.schedule.duration;
        uint8_t weekdays = msg.schedule.weekdays;

        DEBUG_PRINTF("NodeManager: SCHEDULE_SET index=%d ch=%d %02d:%02d %dmin days=0x%02X\n",
                     index, localCh, hour, minute, duration, weekdays);

        // Use updateSchedule if slot exists, otherwise set it up
        if (!_controller->updateSchedule(index, localCh, hour, minute, duration, weekdays)) {
            // Slot wasn't enabled yet — enable it first then update
            _controller->enableSchedule(index, true);
            _controller->updateSchedule(index, localCh, hour, minute, duration, weekdays);
        }
    } else {
        DEBUG_PRINTF("NodeManager: SCHEDULE_SET index=%d CLEAR\n", index);
        _controller->removeSchedule(index);
    }

    // Persist to SPIFFS
    _controller->saveSchedules();

    // Send ACK
    sendAck(senderIp, senderPort, MSG_SCHEDULE_SET, ACK_OK, msg.seq);
}

void NodeManager::handleScheduleAck(const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_MASTER) return;

    DEBUG_PRINTF("NodeManager: SCHEDULE_ACK from '%s' (seq=%d)\n",
                 msg.src_id, msg.ack.acked_seq);
    removeFromOutbox(msg.ack.acked_seq);
}

void NodeManager::handleCmdSkip(IPAddress senderIp, uint16_t senderPort,
                                const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;
    if (!_controller) return;

    uint8_t index = msg.schedule.index;
    DEBUG_PRINTF("NodeManager: CMD_SKIP index=%d\n", index);

    _controller->skipSchedule(index);

    // Send ACK
    sendAck(senderIp, senderPort, MSG_CMD_SKIP, ACK_OK, msg.seq);
}

// ============================================================================
// Utility
// ============================================================================

void NodeManager::fillHeader(IrrigationMsg& msg, uint8_t type,
                             const char* dstId, uint8_t channel) {
    msg.version = NODE_PROTO_VERSION;
    msg.type = type;
    msg.seq = _seq++;
    strncpy(msg.src_id, _nodeId, sizeof(msg.src_id) - 1);
    msg.src_id[sizeof(msg.src_id) - 1] = '\0';
    strncpy(msg.dst_id, dstId, sizeof(msg.dst_id) - 1);
    msg.dst_id[sizeof(msg.dst_id) - 1] = '\0';
    msg.channel = channel;
}

uint32_t NodeManager::hashNodeId(const char* id) {
    // DJB2 hash
    uint32_t hash = 5381;
    while (*id) {
        hash = ((hash << 5) + hash) + (uint8_t)*id++;
    }
    return hash;
}

// ============================================================================
// Auto-Pairing: Master-side handlers
// ============================================================================

void NodeManager::handlePairRequest(IPAddress senderIp, uint16_t senderPort,
                                    const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_MASTER) return;

    const char* srcId = msg.src_id;
    const char* name = msg.pair.name;
    uint8_t numCh = msg.pair.num_channels;

    DEBUG_PRINTF("NodeManager: PAIR_REQUEST from '%s' name='%s' channels=%d IP=%s\n",
                 srcId, name, numCh, senderIp.toString().c_str());

    // Already known slave? Re-send PAIR_ACCEPT (idempotent)
    NodePeer* existing = findSlaveByNodeId(srcId);
    if (existing) {
        DEBUG_PRINTF("NodeManager: '%s' already paired at virtual_ch=%d, re-sending ACCEPT\n",
                     srcId, existing->base_virtual_ch);
        IrrigationMsg reply = {};
        fillHeader(reply, MSG_PAIR_ACCEPT, srcId, 0);
        reply.pair_accept.base_virtual_ch = existing->base_virtual_ch;
        sendUdp(senderIp, senderPort, reply);
        // Update IP in case it changed
        existing->ip = senderIp;
        existing->port = senderPort;
        existing->online = true;
        existing->last_seen = millis();
        return;
    }

    // Already have a pending request? Ignore (one at a time)
    if (_pendingPair.active) {
        DEBUG_PRINTF("NodeManager: Ignoring PAIR_REQUEST from '%s' — already pending '%s'\n",
                     srcId, _pendingPair.node_id);
        return;
    }

    // Slots full?
    if (_slaveCount >= MAX_SLAVES) {
        DEBUG_PRINTLN("NodeManager: Max slaves reached, sending PAIR_REJECT(FULL)");
        IrrigationMsg reply = {};
        fillHeader(reply, MSG_PAIR_REJECT, srcId, 0);
        reply.pair_reject.reason = PAIR_REJECT_FULL;
        sendUdp(senderIp, senderPort, reply);
        return;
    }

    // Store as pending
    strncpy(_pendingPair.node_id, srcId, sizeof(_pendingPair.node_id) - 1);
    _pendingPair.node_id[sizeof(_pendingPair.node_id) - 1] = '\0';
    strncpy(_pendingPair.name, name, sizeof(_pendingPair.name) - 1);
    _pendingPair.name[sizeof(_pendingPair.name) - 1] = '\0';
    _pendingPair.num_channels = numCh;
    _pendingPair.ip = senderIp;
    _pendingPair.port = senderPort;
    _pendingPair.received_at = millis();
    _pendingPair.active = true;

    DEBUG_PRINTF("NodeManager: Pair request stored — waiting for user approval (60s timeout)\n");

    // Fire callback to notify UI (LCD / web)
    if (_pairRequestCallback) {
        _pairRequestCallback(srcId, name);
    }
}

void NodeManager::acceptPendingPair() {
    if (!_pendingPair.active) return;

    uint8_t vch = nextVirtualChannel();
    if (vch == 0) {
        DEBUG_PRINTLN("NodeManager: No virtual channels available, rejecting");
        rejectPendingPair(PAIR_REJECT_FULL);
        return;
    }

    // Register the slave
    addSlave(_pendingPair.node_id, vch);

    // Set name and IP on the newly added peer
    NodePeer* peer = findSlaveByNodeId(_pendingPair.node_id);
    if (peer) {
        strncpy(peer->name, _pendingPair.name, sizeof(peer->name) - 1);
        peer->name[sizeof(peer->name) - 1] = '\0';
        peer->num_channels = _pendingPair.num_channels;
        peer->ip = _pendingPair.ip;
        peer->port = _pendingPair.port;
        peer->online = true;
        peer->last_seen = millis();
    }

    // Persist to SPIFFS
    savePairedSlaves();

    // Send PAIR_ACCEPT
    IrrigationMsg reply = {};
    fillHeader(reply, MSG_PAIR_ACCEPT, _pendingPair.node_id, 0);
    reply.pair_accept.base_virtual_ch = vch;
    sendUdp(_pendingPair.ip, _pendingPair.port, reply);

    DEBUG_PRINTF("NodeManager: Accepted '%s' (%s) at virtual_ch=%d\n",
                 _pendingPair.name, _pendingPair.node_id, vch);

    // Clear pending
    memset(&_pendingPair, 0, sizeof(_pendingPair));
}

void NodeManager::rejectPendingPair(uint8_t reason) {
    if (!_pendingPair.active) return;

    const char* reasonStr = (reason == PAIR_REJECT_FULL) ? "FULL" :
                            (reason == PAIR_REJECT_USER) ? "USER" :
                            (reason == PAIR_REJECT_TIMEOUT) ? "TIMEOUT" : "?";
    DEBUG_PRINTF("NodeManager: Rejecting pair from '%s' reason=%s\n",
                 _pendingPair.node_id, reasonStr);

    IrrigationMsg reply = {};
    fillHeader(reply, MSG_PAIR_REJECT, _pendingPair.node_id, 0);
    reply.pair_reject.reason = reason;
    sendUdp(_pendingPair.ip, _pendingPair.port, reply);

    memset(&_pendingPair, 0, sizeof(_pendingPair));
}

void NodeManager::checkPairTimeout() {
    if (!_pendingPair.active) return;
    if (millis() - _pendingPair.received_at >= NODE_PAIR_REQUEST_TIMEOUT) {
        DEBUG_PRINTLN("NodeManager: Pair request timed out");
        rejectPendingPair(PAIR_REJECT_TIMEOUT);
    }
}

uint8_t NodeManager::nextVirtualChannel() {
    uint8_t next = NUM_LOCAL_CHANNELS + 1;  // First virtual channel (e.g. 7)
    for (uint8_t i = 0; i < _slaveCount; i++) {
        uint8_t end = _slaves[i].base_virtual_ch + _slaves[i].num_channels;
        if (end > next) next = end;
    }
    if (next > MAX_CHANNELS) return 0;  // Full
    return next;
}

bool NodeManager::unpairSlave(const char* nodeId) {
    int idx = -1;
    for (uint8_t i = 0; i < _slaveCount; i++) {
        if (strncmp(_slaves[i].node_id, nodeId, sizeof(_slaves[i].node_id)) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return false;

    // Clear virtual channel status on controller
    if (_controller) {
        for (uint8_t ch = 0; ch < _slaves[idx].num_channels; ch++) {
            _controller->setRemoteChannelStatus(
                _slaves[idx].base_virtual_ch + ch, false, 0);
        }
    }

    DEBUG_PRINTF("NodeManager: Unpaired slave '%s' (virtual_ch=%d)\n",
                 _slaves[idx].node_id, _slaves[idx].base_virtual_ch);

    // Shift remaining slaves down
    for (uint8_t i = idx; i < _slaveCount - 1; i++) {
        _slaves[i] = _slaves[i + 1];
    }
    _slaveCount--;
    memset(&_slaves[_slaveCount], 0, sizeof(NodePeer));

    savePairedSlaves();
    return true;
}

bool NodeManager::renameSlave(const char* nodeId, const char* newName) {
    NodePeer* peer = findSlaveByNodeId(nodeId);
    if (!peer) return false;

    strncpy(peer->name, newName, sizeof(peer->name) - 1);
    peer->name[sizeof(peer->name) - 1] = '\0';

    savePairedSlaves();
    DEBUG_PRINTF("NodeManager: Renamed slave '%s' to '%s'\n", nodeId, newName);
    return true;
}

// ============================================================================
// Auto-Pairing: Slave-side handlers
// ============================================================================

void NodeManager::sendPairRequest() {
    if (!_masterFound) return;

    // Use master's node_id if known, otherwise broadcast
    // (before pairing, _masterNodeId may be empty since we haven't received a heartbeat yet)
    const char* dstId = (_masterNodeId[0] != '\0') ? _masterNodeId : NODE_BROADCAST_ID;

    IrrigationMsg msg = {};
    fillHeader(msg, MSG_PAIR_REQUEST, dstId, 0);
    msg.pair.num_channels = NUM_LOCAL_CHANNELS;
    strncpy(msg.pair.name, _nodeName, sizeof(msg.pair.name) - 1);
    msg.pair.name[sizeof(msg.pair.name) - 1] = '\0';

    DEBUG_PRINTF("NodeManager: Sending PAIR_REQUEST to master (name='%s', channels=%d, dst='%s')\n",
                 _nodeName, NUM_LOCAL_CHANNELS, dstId);
    sendUdp(_masterIp, _masterPort, msg);
}

void NodeManager::handlePairAccept(const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;

    _assignedVirtualCh = msg.pair_accept.base_virtual_ch;
    _paired = true;

    // Save master info
    strncpy(_masterNodeId, msg.src_id, sizeof(_masterNodeId) - 1);
    _masterNodeId[sizeof(_masterNodeId) - 1] = '\0';

    savePairedMaster();

    DEBUG_PRINTF("NodeManager: PAIR_ACCEPT! Paired with master '%s', virtual_ch=%d\n",
                 _masterNodeId, _assignedVirtualCh);
}

void NodeManager::handlePairReject(const IrrigationMsg& msg) {
    if (_role != NODE_ROLE_SLAVE) return;

    const char* reasonStr = (msg.pair_reject.reason == PAIR_REJECT_FULL) ? "FULL" :
                            (msg.pair_reject.reason == PAIR_REJECT_USER) ? "USER" :
                            (msg.pair_reject.reason == PAIR_REJECT_TIMEOUT) ? "TIMEOUT" : "?";
    DEBUG_PRINTF("NodeManager: PAIR_REJECT reason=%s — will retry in %ds\n",
                 reasonStr, NODE_PAIR_RETRY_INTERVAL / 1000);
    // _lastPairAttempt is already set, so next attempt will be after the interval
}

// ============================================================================
// Auto-Pairing: SPIFFS persistence
// ============================================================================

void NodeManager::savePairedSlaves() {
    DynamicJsonDocument doc(1024);

    for (uint8_t i = 0; i < _slaveCount; i++) {
        JsonObject slave = doc.createNestedObject(_slaves[i].node_id);
        slave["virtual_channel"] = _slaves[i].base_virtual_ch;
        slave["name"] = _slaves[i].name;
        slave["num_channels"] = _slaves[i].num_channels;
    }

    File file = SPIFFS.open(PAIRED_SLAVES_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("NodeManager: Failed to open paired_slaves.json for writing");
        return;
    }
    serializeJson(doc, file);
    file.close();
    DEBUG_PRINTF("NodeManager: Saved %d paired slaves to SPIFFS\n", _slaveCount);
}

void NodeManager::loadPairedSlaves() {
    if (!SPIFFS.exists(PAIRED_SLAVES_FILE)) {
        DEBUG_PRINTLN("NodeManager: No paired_slaves.json found");
        return;
    }

    File file = SPIFFS.open(PAIRED_SLAVES_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("NodeManager: Failed to open paired_slaves.json");
        return;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("NodeManager: Failed to parse paired_slaves.json: %s\n", error.c_str());
        return;
    }

    for (JsonPair kv : doc.as<JsonObject>()) {
        const char* nodeId = kv.key().c_str();
        uint8_t vch = kv.value()["virtual_channel"] | 0;
        const char* name = kv.value()["name"] | "";
        uint8_t numCh = kv.value()["num_channels"] | 1;

        if (vch == 0 || strlen(nodeId) == 0) continue;

        addSlave(nodeId, vch);

        // Set name and num_channels on the peer
        NodePeer* peer = findSlaveByNodeId(nodeId);
        if (peer) {
            strncpy(peer->name, name, sizeof(peer->name) - 1);
            peer->name[sizeof(peer->name) - 1] = '\0';
            peer->num_channels = numCh;
        }

        DEBUG_PRINTF("NodeManager: Loaded paired slave '%s' (%s) virtual_ch=%d\n",
                     name, nodeId, vch);
    }
}

void NodeManager::savePairedMaster() {
    StaticJsonDocument<256> doc;
    doc["master_id"] = _masterNodeId;
    doc["virtual_channel"] = _assignedVirtualCh;

    File file = SPIFFS.open(PAIRED_MASTER_FILE, "w");
    if (!file) {
        DEBUG_PRINTLN("NodeManager: Failed to open paired_master.json for writing");
        return;
    }
    serializeJson(doc, file);
    file.close();
    DEBUG_PRINTF("NodeManager: Saved pairing to SPIFFS (master=%s, vch=%d)\n",
                 _masterNodeId, _assignedVirtualCh);
}

void NodeManager::loadPairedMaster() {
    if (!SPIFFS.exists(PAIRED_MASTER_FILE)) {
        DEBUG_PRINTLN("NodeManager: No paired_master.json found — will request pairing");
        _paired = false;
        return;
    }

    File file = SPIFFS.open(PAIRED_MASTER_FILE, "r");
    if (!file) {
        DEBUG_PRINTLN("NodeManager: Failed to open paired_master.json");
        _paired = false;
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        DEBUG_PRINTF("NodeManager: Failed to parse paired_master.json: %s\n", error.c_str());
        _paired = false;
        return;
    }

    const char* masterId = doc["master_id"] | "";
    _assignedVirtualCh = doc["virtual_channel"] | 0;

    if (strlen(masterId) > 0 && _assignedVirtualCh > 0) {
        strncpy(_masterNodeId, masterId, sizeof(_masterNodeId) - 1);
        _masterNodeId[sizeof(_masterNodeId) - 1] = '\0';
        _paired = true;
        DEBUG_PRINTF("NodeManager: Loaded pairing — master='%s', virtual_ch=%d\n",
                     _masterNodeId, _assignedVirtualCh);
    } else {
        _paired = false;
        DEBUG_PRINTLN("NodeManager: Invalid paired_master.json — will request pairing");
    }
}
