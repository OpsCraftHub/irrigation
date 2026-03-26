#ifndef NODE_MANAGER_H
#define NODE_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include "Config.h"
#include "NodeProtocol.h"

// Forward declaration
class IrrigationController;

#define OUTBOX_SIZE 8
#define DEDUP_SIZE 16

// Peer state (master-side bookkeeping for each slave)
struct NodePeer {
    char node_id[12];
    char name[16];             // Human-readable name from PAIR_REQUEST
    IPAddress ip;
    uint16_t port;
    uint8_t base_virtual_ch;   // First virtual channel on master (e.g. 7)
    uint8_t num_channels;      // How many channels the slave has
    bool online;
    unsigned long last_seen;   // millis() of last message
    bool irrigating;
    uint16_t time_remaining;   // seconds
    int8_t rssi;
};

// Pending pair request (master holds one at a time)
struct PendingPairRequest {
    char node_id[12];
    char name[16];
    uint8_t num_channels;
    IPAddress ip;
    uint16_t port;
    unsigned long received_at;
    bool active;
};

// Callback when a pair request arrives (master-side)
typedef void (*PairRequestCallback)(const char* nodeId, const char* name);

// Outbox entry for reliable command delivery
struct OutboxEntry {
    IrrigationMsg msg;
    IPAddress dst_ip;
    uint16_t dst_port;
    unsigned long last_send;
    uint8_t retries;
    bool active;
};

// Dedup entry to ignore duplicate messages
struct DedupEntry {
    uint16_t seq;
    uint32_t src_hash;
    unsigned long timestamp;
};

class NodeManager {
public:
    NodeManager();
    ~NodeManager();

    // Component lifecycle
    bool begin();
    void update();

    // Configuration
    void setRole(uint8_t role) { _role = role; }
    uint8_t getRole() const { return _role; }
    void setController(IrrigationController* ctrl) { _controller = ctrl; }
    void setNodeId(const char* id);

    // Master: register a slave peer by node_id
    bool addSlave(const char* nodeId, uint8_t baseVirtualCh);

    // Master: send command to a virtual channel
    bool sendStart(uint8_t virtualChannel, uint16_t durationMinutes);
    bool sendStop(uint8_t virtualChannel);

    // Slave peer info (for master)
    const NodePeer* getSlave(uint8_t index) const;
    uint8_t getSlaveCount() const { return _slaveCount; }

    // Auto-pairing (slave)
    void setNodeName(const char* name);
    bool isPaired() const { return _paired; }

    // Auto-pairing (master)
    void setPairRequestCallback(PairRequestCallback cb) { _pairRequestCallback = cb; }
    bool hasPendingPair() const { return _pendingPair.active; }
    const PendingPairRequest& getPendingPair() const { return _pendingPair; }
    void acceptPendingPair();
    void rejectPendingPair(uint8_t reason);

    // Unpair a slave by node_id (master)
    bool unpairSlave(const char* nodeId);

    // Rename a paired slave (master)
    bool renameSlave(const char* nodeId, const char* newName);

private:
    // UDP transport
    bool sendUdp(IPAddress ip, uint16_t port, const IrrigationMsg& msg);
    void receiveUdp();

    // mDNS
    void advertiseMdns();
    bool discoverMaster();

    // Reliability layer
    void enqueueOutbox(const IrrigationMsg& msg, IPAddress ip, uint16_t port);
    void processOutbox();
    void removeFromOutbox(uint16_t seq);
    bool isDuplicate(const char* srcId, uint16_t seq);
    void addDedup(const char* srcId, uint16_t seq);

    // Message handlers
    void handleMessage(IPAddress senderIp, uint16_t senderPort,
                       const uint8_t* data, int len);
    void handleCmdStart(IPAddress senderIp, uint16_t senderPort,
                        const IrrigationMsg& msg);
    void handleCmdStop(IPAddress senderIp, uint16_t senderPort,
                       const IrrigationMsg& msg);
    void handleCmdAck(const IrrigationMsg& msg);
    void handleStatus(IPAddress senderIp, const IrrigationMsg& msg);
    void handleHeartbeat(IPAddress senderIp, uint16_t senderPort,
                         const IrrigationMsg& msg);
    void handleHeartbeatAck(const IrrigationMsg& msg);

    // Pairing handlers
    void handlePairRequest(IPAddress senderIp, uint16_t senderPort,
                           const IrrigationMsg& msg);
    void handlePairAccept(const IrrigationMsg& msg);
    void handlePairReject(const IrrigationMsg& msg);
    void sendPairRequest();
    uint8_t nextVirtualChannel();
    void checkPairTimeout();

    // Pairing persistence
    void savePairedSlaves();
    void loadPairedSlaves();
    void savePairedMaster();
    void loadPairedMaster();

    // Sending helpers
    void sendHeartbeat();
    void sendStatus();
    void sendAck(IPAddress ip, uint16_t port, uint8_t ackedType,
                 uint8_t result, uint16_t ackedSeq);

    // Peer lookup
    NodePeer* findSlaveByNodeId(const char* nodeId);
    NodePeer* findSlaveByVirtualCh(uint8_t virtualCh);

    // Timeout check
    void checkPeerTimeouts();

    // Utility
    void fillHeader(IrrigationMsg& msg, uint8_t type, const char* dstId,
                    uint8_t channel);
    uint32_t hashNodeId(const char* id);

    // Members
    uint8_t _role;
    char _nodeId[12];
    IrrigationController* _controller;
    uint16_t _seq;

    WiFiUDP _udp;

    // Master: slave peers
    NodePeer _slaves[MAX_SLAVES];
    uint8_t _slaveCount;

    // Slave: master info
    IPAddress _masterIp;
    uint16_t _masterPort;
    char _masterNodeId[12];
    bool _masterFound;
    unsigned long _lastMdnsQuery;

    // Auto-pairing state
    PendingPairRequest _pendingPair;       // Master: current pending request
    PairRequestCallback _pairRequestCallback;
    bool _paired;                          // Slave: paired with a master
    uint8_t _assignedVirtualCh;            // Slave: my virtual channel
    unsigned long _lastPairAttempt;        // Slave: last PAIR_REQUEST send time
    char _nodeName[16];                    // Slave: human-readable name

    // mDNS state
    bool _mdnsStarted;

    // Reliability
    OutboxEntry _outbox[OUTBOX_SIZE];
    DedupEntry _dedup[DEDUP_SIZE];
    uint8_t _dedupIdx;

    // Timing
    unsigned long _lastHeartbeat;
    unsigned long _lastStatusSend;
    bool _initialized;
};

#endif // NODE_MANAGER_H
