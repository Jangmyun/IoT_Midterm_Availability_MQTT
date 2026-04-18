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
// - Nodes in both: newer CT snapshot should replace the local node row
// - Links: upsert via addLink (rtt updated if exists)
// Returns true if local CT was modified.
inline bool same_node_entry(const NodeEntry& left, const NodeEntry& right) {
    return std::strncmp(left.id, right.id, UUID_LEN) == 0 &&
           left.role == right.role &&
           std::strncmp(left.ip, right.ip, IP_LEN) == 0 &&
           left.port == right.port &&
           left.status == right.status &&
           left.hop_to_core == right.hop_to_core;
}

inline bool merge_connection_tables(ConnectionTableManager& local,
                                    const ConnectionTable& remote) {
    bool changed = false;
    for (int i = 0; i < remote.node_count; i++) {
        const NodeEntry& rn = remote.nodes[i];
        auto existing = local.findNode(rn.id);
        if (!existing.has_value()) {
            local.addNode(rn);
            changed = true;
        } else if (!same_node_entry(*existing, rn)) {
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

    for (int i = 0; i < remote.node_count; i++) {
        const NodeEntry& rn = remote.nodes[i];
        auto existing = local.findNode(rn.id);

        if (!existing.has_value()) {
            local.addNode(rn);
            changed = true;
            continue;
        }

        // Active Core는 edge liveness의 기준이다.
        // Backup의 stale snapshot 이 ONLINE edge를 OFFLINE으로 덮어쓰면 안 된다.
        if (rn.role == NODE_ROLE_NODE &&
            existing->status == NODE_STATUS_ONLINE &&
            rn.status == NODE_STATUS_OFFLINE) {
            continue;
        }

        if (!same_node_entry(*existing, rn)) {
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

inline bool promote_core_after_failover(ConnectionTableManager& ct_manager,
                                        const char* promoted_core_id,
                                        const char* failed_core_id) {
    bool changed = false;

    if (failed_core_id && failed_core_id[0] != '\0' &&
        std::strncmp(failed_core_id, promoted_core_id, UUID_LEN) != 0) {
        auto failed_node = ct_manager.findNode(failed_core_id);
        if (failed_node.has_value() && failed_node->status != NODE_STATUS_OFFLINE) {
            changed = ct_manager.setNodeStatus(failed_core_id, NODE_STATUS_OFFLINE) || changed;
        }
    }

    ConnectionTable snapshot = ct_manager.snapshot();

    if (promoted_core_id && promoted_core_id[0] != '\0' &&
        std::strncmp(snapshot.active_core_id, promoted_core_id, UUID_LEN) != 0) {
        ct_manager.setActiveCoreId(promoted_core_id);
        changed = true;
    }

    if (snapshot.backup_core_id[0] != '\0') {
        ct_manager.setBackupCoreId("");
        changed = true;
    }

    return changed;
}

inline bool should_promote_backup_on_core_will(const ConnectionTableManager& ct_manager,
                                               const char* backup_core_id,
                                               const char* failed_core_id) {
    if (!failed_core_id || failed_core_id[0] == '\0') {
        return false;
    }

    if (backup_core_id && backup_core_id[0] != '\0' &&
        std::strncmp(backup_core_id, failed_core_id, UUID_LEN) == 0) {
        return false;
    }

    ConnectionTable snapshot = ct_manager.snapshot();
    if (snapshot.active_core_id[0] == '\0') {
        return true;
    }

    return std::strncmp(snapshot.active_core_id, failed_core_id, UUID_LEN) == 0;
}
