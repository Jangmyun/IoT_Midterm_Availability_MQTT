#include "connection_table_manager.h"
#include <cstring>
#include <ctime>
#include <cstdio>

// Util =====================================================

// Curren Time 을 문자열로
static void set_now(char* buf) {
    std::time_t t = std::time(nullptr);
    struct tm* utc = std::gmtime(&t);
    std::strftime(buf, TIMESTAMP_LEN, "%Y-%m-%dT%H:%M:%SZ", utc);
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
    table_.nodes[table_.node_count++] = node;
    bumpVersion();
    return true;
}

// UUID 에 해당하는 노드정보를 업데이트
bool ConnectionTableManager::updateNode(const NodeEntry& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < table_.node_count; i++) { // 반복문 내에서 문자열 비교
        if (std::strncmp(table_.nodes[i].id, node.id, UUID_LEN) == 0) {
            table_.nodes[i] = node;
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
            table_.nodes[i].status = status;
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
