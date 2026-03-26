#ifndef NODE_PROTOCOL_H
#define NODE_PROTOCOL_H

#include <stdint.h>

// Protocol version — increment on breaking changes
// v2: UDP transport, src_id/dst_id addressing (replaces ESP-NOW + MAC addressing)
#define NODE_PROTO_VERSION 2

// Message types (aligned with PRODUCT_SPEC.md section 5.1)
#define MSG_HEARTBEAT       0x01
#define MSG_HEARTBEAT_ACK   0x02
#define MSG_SCHEDULE_SET    0x10  // future
#define MSG_SCHEDULE_ACK    0x11  // future
#define MSG_SCHEDULE_REQ    0x12  // future
#define MSG_CMD_START       0x20
#define MSG_CMD_STOP        0x21
#define MSG_CMD_SKIP        0x22  // future
#define MSG_CMD_EXTRA       0x23  // future
#define MSG_CMD_ACK         0x2F
#define MSG_STATUS          0x30
#define MSG_PAIR_REQUEST    0x40
#define MSG_PAIR_ACCEPT     0x41
#define MSG_PAIR_REJECT     0x42
// 0x70-0x7F reserved for future sensor node payloads

// ACK result codes
#define ACK_OK            0x00
#define ACK_ERR_CHANNEL   0x01
#define ACK_ERR_BUSY      0x02

// Pair reject reasons
#define PAIR_REJECT_FULL    0x01
#define PAIR_REJECT_USER    0x02
#define PAIR_REJECT_TIMEOUT 0x03

// Node roles
#define NODE_ROLE_MASTER  0x01
#define NODE_ROLE_SLAVE   0x02

// Broadcast destination
#define NODE_BROADCAST_ID "*"

// Binary message: 29-byte header + up to 20-byte payload
typedef struct __attribute__((packed)) {
    // Header (29 bytes)
    uint8_t  version;        // NODE_PROTO_VERSION
    uint8_t  type;           // MSG_* constant
    uint16_t seq;            // sequence number for ACK/dedup
    char     src_id[12];     // source node_id (null-terminated, max 11 chars)
    char     dst_id[12];     // destination node_id ("*" = broadcast)
    uint8_t  channel;        // channel (1-based), 0 = N/A, 0xFF = all

    // Payload (max 20 bytes)
    union {
        struct {                          // MSG_CMD_START (2 bytes)
            uint16_t duration;           // minutes (0 = use schedule default)
        } command;

        struct {                          // MSG_STATUS (8 bytes)
            uint8_t  state;              // 0=idle, 1=irrigating, 2=error
            uint16_t time_remaining;     // seconds
            uint16_t flow_litres;        // x10 for 0.1L resolution
            uint8_t  battery_pct;        // 0-100, 0xFF = mains
            uint8_t  tank_pct;           // 0-100, 0xFF = no sensor
            int8_t   rssi;               // WiFi signal dBm
        } status;

        struct {                          // MSG_HEARTBEAT (7 bytes)
            uint8_t  num_channels;
            uint8_t  role;               // NODE_ROLE_*
            uint8_t  pending_cmds;       // queued commands count
            uint32_t uptime;             // seconds
        } heartbeat;

        struct {                          // MSG_HEARTBEAT_ACK (4 bytes)
            uint32_t epoch_time;         // current epoch from master (NTP)
        } heartbeat_ack;

        struct {                          // MSG_CMD_ACK (4 bytes)
            uint8_t  acked_type;         // MSG_* of acknowledged message
            uint8_t  result;             // ACK_* result code
            uint16_t acked_seq;          // seq of acknowledged message
        } ack;

        struct {                          // MSG_SCHEDULE_SET (7 bytes, future)
            uint8_t  index;
            uint8_t  enabled;
            uint8_t  hour;
            uint8_t  minute;
            uint16_t duration;
            uint8_t  weekdays;
        } schedule;

        struct {                          // MSG_PAIR_REQUEST (17 bytes)
            uint8_t  num_channels;
            char     name[16];
        } pair;

        struct {                          // MSG_PAIR_ACCEPT (1 byte)
            uint8_t  base_virtual_ch;
        } pair_accept;

        struct {                          // MSG_PAIR_REJECT (1 byte)
            uint8_t  reason;
        } pair_reject;

        uint8_t raw[20];                 // generic access
    };
} IrrigationMsg;

#endif // NODE_PROTOCOL_H
