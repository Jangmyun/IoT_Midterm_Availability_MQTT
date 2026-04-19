#include "connection_table_manager.h"
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cmath>

// Util =====================================================

// Curren Time 을 문자열로
static void set_now(char* buf) {
    std::time_t t = std::time(nullptr);
    struct tm* utc = std::gmtime(&t);
    std::strftime(buf, TIMESTAMP_LEN, "%Y-%m-%dT%H:%M:%SZ", utc);
}

static void copy_timestamp(char* dst, const char* src) {
    if (!dst) return;
    std::memset(dst, 0, TIMESTAMP_LEN);
    if (!src || src[0] == '\0') return;
    std::strncpy(dst, src, TIMESTAMP_LEN - 1);
    dst[TIMESTAMP_LEN - 1] = '\0';
}

// Public

// Initialize ConnectionTable - memory allocation, set current time
void ConnectionTableManager::init(const char* active_core_id, const char* backup_core_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memset(&table_, 0, sizeof(table_));
    set_now(table_.last_update);
    std::strncpy(table_.active_core_id, active_core_id, UUID_LEN - 1);
    if (backup_core_id && backup_core_id[0] != '\0') {
        std::strncpy(table_.backup_core_id, backup_core_id, UUID_LEN - 1);
    }
}

// Node =====================================================

// 노드를 connection table에 추가 시 노드의 version 증가
bool ConnectionTableManager::addNode(const NodeEntry& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.node_count >= MAX_NODES) return false;
    for (int i = 0; i < table_.node_count; i++) {
        if (std::strncmp(table_.nodes[i].id, node.id, UUID_LEN) == 0) return false;
    }
    NodeEntry stored = node;
    if (stored.status_changed_at[0] == '\0') {
        stored.previous_status = stored.status;
        set_now(stored.status_changed_at);
    }
    table_.nodes[table_.node_count++] = stored;
    bumpVersion();
    return true;
}

// UUID 에 해당하는 노드정보를 업데이트
bool ConnectionTableManager::updateNode(const NodeEntry& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.node_count; i++) { // 반복문 내에서 문자열 비교
        if (std::strncmp(table_.nodes[i].id, node.id, UUID_LEN) == 0) {
            NodeEntry next = node;
            if (next.status_changed_at[0] == '\0') {
                if (table_.nodes[i].status != next.status) {
                    next.previous_status = table_.nodes[i].status;
                    set_now(next.status_changed_at);
                } else {
                    next.previous_status = table_.nodes[i].previous_status;
                    copy_timestamp(next.status_changed_at, table_.nodes[i].status_changed_at);
                }
            }
            table_.nodes[i] = next;
            bumpVersion();
            return true;
        }
    }
    return false;
}

// UUID 에 해당하는 노드의 status 설정
bool ConnectionTableManager::setNodeStatus(const char* id, NodeStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.node_count; i++) {
        if (std::strncmp(table_.nodes[i].id, id, UUID_LEN) == 0) {
            if (table_.nodes[i].status == status) {
                return false;
            }
            table_.nodes[i].previous_status = table_.nodes[i].status;
            table_.nodes[i].status = status;
            set_now(table_.nodes[i].status_changed_at);
            bumpVersion();
            return true;
        }
    }
    return false;
}

// UUID로 노드 찾기
std::optional<NodeEntry> ConnectionTableManager::findNode(const char* id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.node_count; i++) {
        if (std::strncmp(table_.nodes[i].id, id, UUID_LEN) == 0) {
            return table_.nodes[i];
        }
    }
    return std::nullopt;
}

// Link =====================================================

// 
bool ConnectionTableManager::addLink(const LinkEntry& link) {
    std::lock_guard<std::mutex> lock(mutex_);
    // (from, to) 쌍이 이미 존재하면 RTT만 갱신
    for (int i = 0; i < table_.link_count; i++) {
        if (std::strncmp(table_.links[i].from_id, link.from_id, UUID_LEN) == 0 &&
            std::strncmp(table_.links[i].to_id, link.to_id, UUID_LEN) == 0) {
            if (std::fabs(table_.links[i].rtt_ms - link.rtt_ms) < 0.0001f) {
                return false;
            }
            table_.links[i].rtt_ms = link.rtt_ms;
            bumpVersion();
            return true;
        }
    }
    if (table_.link_count >= MAX_LINKS) return false;
    table_.links[table_.link_count++] = link;
    bumpVersion();
    return true;
}


bool ConnectionTableManager::updateLinkRtt(const char* from_id, const char* to_id, float rtt_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.link_count; i++) {
        if (std::strncmp(table_.links[i].from_id, from_id, UUID_LEN) == 0 &&
            std::strncmp(table_.links[i].to_id, to_id, UUID_LEN) == 0) {
            table_.links[i].rtt_ms = rtt_ms;
            bumpVersion();
            return true;
        }
    }
    return false;
}

std::optional<LinkEntry> ConnectionTableManager::findLink(const char* from_id, const char* to_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.link_count; i++) {
        if (std::strncmp(table_.links[i].from_id, from_id, UUID_LEN) == 0 &&
            std::strncmp(table_.links[i].to_id, to_id, UUID_LEN) == 0) {
            return table_.links[i];
        }
    }
    return std::nullopt;
}

// Core ID =====================================================


void ConnectionTableManager::setActiveCoreId(const char* id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::strncmp(table_.active_core_id, id, UUID_LEN) == 0) {
        return;
    }
    std::strncpy(table_.active_core_id, id, UUID_LEN - 1);
    table_.active_core_id[UUID_LEN - 1] = '\0';
    bumpVersion();
}

void ConnectionTableManager::setBackupCoreId(const char* id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::strncmp(table_.backup_core_id, id, UUID_LEN) == 0) {
        return;
    }
    std::strncpy(table_.backup_core_id, id, UUID_LEN - 1);
    table_.backup_core_id[UUID_LEN - 1] = '\0';
    bumpVersion();
}

void ConnectionTableManager::replace(const ConnectionTable& table) {
    std::lock_guard<std::mutex> lock(mutex_);
    table_ = table;
}

// Serialization =====================================================

ConnectionTable ConnectionTableManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_;
}

// Private =====================================================

void ConnectionTableManager::bumpVersion() {
    table_.version++;
    set_now(table_.last_update);
}
