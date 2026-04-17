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

// Merge Backup Core registration into Active Core's CT.
// Unlike CT_SYNC, this path must accept equal versions during bootstrap because:
// - Active starts with version=1 (self node only)
// - Backup starts with version=1 (self node + backup_core_id)
// The merge must still learn backup_core_id and backup node on first registration.
inline bool merge_backup_registration(ConnectionTableManager& local,
                                      const char* active_core_id,
                                      const ConnectionTable& remote) {
    bool changed = false;

    if (remote.backup_core_id[0] != '\0') {
        ConnectionTable local_snapshot = local.snapshot();
        if (std::strncmp(local_snapshot.backup_core_id, remote.backup_core_id, UUID_LEN) != 0) {
            local.setBackupCoreId(remote.backup_core_id);
            changed = true;
        }

        if (active_core_id && active_core_id[0] != '\0') {
            LinkEntry link = {};
            std::strncpy(link.from_id, active_core_id, UUID_LEN - 1);
            std::strncpy(link.to_id, remote.backup_core_id, UUID_LEN - 1);
            link.rtt_ms = 0.0f;
            if (local.addLink(link)) {
                changed = true;
            }
        }
    }

    if (merge_connection_tables(local, remote)) {
        changed = true;
    }

    return changed;
}
