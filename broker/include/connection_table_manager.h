#pragma once

#include <mutex>
#include <optional>
#include "connection_table.h"

class ConnectionTableManager {
public:
    // version=0으로 초기화, active/backup core ID 설정
    void init(const char* active_core_id, const char* backup_core_id = "");

    // Node 조작 — ID 중복 시 addNode false, ID 없으면 updateNode/setNodeStatus false
    bool addNode(const NodeEntry& node);
    bool updateNode(const NodeEntry& node);
    bool setNodeStatus(const char* id, NodeStatus status);
    std::optional<NodeEntry> findNode(const char* id) const;

    // Link 조작 — (from_id, to_id) 쌍이 이미 존재하면 RTT만 갱신
    bool addLink(const LinkEntry& link);
    bool updateLinkRtt(const char* from_id, const char* to_id, float rtt_ms);
    std::optional<LinkEntry> findLink(const char* from_id, const char* to_id) const;

    // Core failover 시 active/backup 교체
    void setActiveCoreId(const char* id);
    void setBackupCoreId(const char* id);

    // 직렬화용 스냅샷 (thread-safe 복사본 반환)
    ConnectionTable snapshot() const;

private:
    // 모든 mutation 후 내부적으로 호출 — version++ 및 last_update 갱신
    // 반드시 mutex_ 보유 상태에서 호출할 것
    void bumpVersion();

    ConnectionTable    table_;
    mutable std::mutex mutex_;
};
