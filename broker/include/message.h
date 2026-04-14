#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "connection_table.h"

// String Buffer Sizes
#define MSG_TYPE_LEN      32   /* "INTRUSION\0" etc. */
#define PRIORITY_LEN      8    /* "HIGH\0"           */
#define DESCRIPTION_LEN   256
#define BUILDING_ID_LEN   64
#define CAMERA_ID_LEN     64

// Message Type
typedef enum {
    MSG_TYPE_MOTION,
    MSG_TYPE_DOOR_FORCED,
    MSG_TYPE_INTRUSION,
    MSG_TYPE_RELAY,
    MSG_TYPE_PING_REQUEST,
    MSG_TYPE_PING_RESPONSE,
    MSG_TYPE_STATUS,
    MSG_TYPE_LWT_CORE,
    MSG_TYPE_LWT_NODE,
    MSG_TYPE_UNKNOWN
} MsgType;

// Message Priority
typedef enum {
    PRIORITY_HIGH,
    PRIORITY_MEDIUM,
    PRIORITY_LOW,
    PRIORITY_NONE   /* field omitted */
} MsgPriority;

// Source / Target EndPoint
typedef struct {
    NodeRole role;          /* NODE_ROLE_NODE or NODE_ROLE_CORE */
    char     id[UUID_LEN];
} Endpoint;

// Route
typedef struct {
    char original_node[UUID_LEN];
    char prev_hop[UUID_LEN];
    char next_hop[UUID_LEN];  /* empty string → next hop is Core */
    int  hop_count;
    int  ttl;
} RouteInfo;

// Delivery
typedef struct {
    int  qos;    /* 0 / 1 / 2 */
    bool dup;
    bool retain;
} DeliveryInfo;

// Event Payload
typedef struct {
    char building_id[BUILDING_ID_LEN];
    char camera_id[CAMERA_ID_LEN];
    char description[DESCRIPTION_LEN];
} EventPayload;

// MQTT Payload (Top-Level Message)
typedef struct {
    char         msg_id[UUID_LEN];
    MsgType      type;
    char         timestamp[TIMESTAMP_LEN];
    MsgPriority  priority;    /* PRIORITY_NONE if omitted */
    Endpoint     source;
    Endpoint     target;
    RouteInfo    route;
    DeliveryInfo delivery;
    EventPayload payload;
} MqttMessage;
