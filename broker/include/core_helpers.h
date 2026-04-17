#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "connection_table_manager.h"

// Parse "ip:port" string (stored in MqttMessage::payload.description for edge registration)
// Returns false if format is invalid or port is out of range
inline bool parse_ip_port(const char* desc, char* ip_out, size_t ip_len, int* port_out) {
    char ip_buf[64] = {};
    int  port = 0;
    if (sscanf(desc, "%63[^:]:%d", ip_buf, &port) != 2) return false;
    if (port <= 0 || port > 65535) return false;
    snprintf(ip_out, ip_len, "%s", ip_buf);
    *port_out = port;
    return true;
}

// Build an alert topic: "<prefix>/<id>"  e.g. "campus/alert/node_down/<uuid>"
inline void make_alert_topic(const char* prefix, const char* id, char* buf, size_t len) {
    snprintf(buf, len, "%s/%s", prefix, id);
}

// Merge remote ConnectionTable into local ConnectionTableManager.
// - Nodes in remote not present in local → addNode
// - Nodes in both: prefer ONLINE over OFFLINE
// - Links: upsert via addLink (rtt updated if exists)
// Returns true if local CT was modified.
inline bool merge_connection_tables(ConnectionTableManager& local,
                                    const ConnectionTable& remote) {
    bool changed = false;
    for (int i = 0; i < remote.node_count; i++) {
        const NodeEntry& rn = remote.nodes[i];
        auto existing = local.findNode(rn.id);
        if (!existing.has_value()) {
            local.addNode(rn);
            changed = true;
        } else if (rn.status == NODE_STATUS_ONLINE &&
                   existing->status == NODE_STATUS_OFFLINE) {
            local.updateNode(rn);
            changed = true;
        }
    }
    for (int i = 0; i < remote.link_count; i++) {
        if (local.addLink(remote.links[i])) {
            changed = true;
        }
    }
    return changed;
}
