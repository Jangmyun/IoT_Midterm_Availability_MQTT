#pragma once

#include <stdint.h>

#define UUID_LEN       37   /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */
#define IP_LEN         16   /* "xxx.xxx.xxx.xxx\0" */
#define TIMESTAMP_LEN  25   /* ISO 8601 UTC e.g. "2025-04-12T09:31:00Z\0" */
#define MAX_NODES      64
#define MAX_LINKS      (MAX_NODES * 4)

typedef enum {
    NODE_ROLE_NODE,
    NODE_ROLE_CORE
} NodeRole;

typedef enum {
    NODE_STATUS_ONLINE,
    NODE_STATUS_OFFLINE
} NodeStatus;

typedef struct {
    char       id[UUID_LEN];
    NodeRole   role;
    char       ip[IP_LEN];
    uint16_t   port;
    NodeStatus status;
    int        hop_to_core;
} NodeEntry;

typedef struct {
    char  from_id[UUID_LEN];
    char  to_id[UUID_LEN];
    float rtt_ms;
} LinkEntry;

typedef struct {
    int        version;
    char       last_update[TIMESTAMP_LEN];
    char       active_core_id[UUID_LEN];
    char       backup_core_id[UUID_LEN];
    NodeEntry  nodes[MAX_NODES];
    int        node_count;
    LinkEntry  links[MAX_LINKS];
    int        link_count;
} ConnectionTable;
