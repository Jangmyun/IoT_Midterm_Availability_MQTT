#pragma once
// edge_upstream.h
// Upstream 연결 배열 구조체 및 순수 헬퍼 함수
// mosquitto.h 불필요 (struct mosquitto 는 forward declaration 만 사용)
// → test_edge_logic.cpp 에서도 include 가능

#include "connection_table.h"  // IP_LEN, UUID_LEN

struct mosquitto;  // forward declaration

static constexpr int MAX_UPSTREAM = 4;

enum class UpstreamKind { CORE, BACKUP, PEER_EDGE };

struct UpstreamConn {
    struct mosquitto* mosq               = nullptr;
    UpstreamKind      kind               = UpstreamKind::CORE;
    char              ip[IP_LEN]         = {};
    uint16_t          port               = 0;
    bool              connected          = false;
    bool              preferred          = false;  // 페일오버 우선 플래그 (prefer_backup 대체)
    char              peer_node_id[UUID_LEN] = {};  // PEER_EDGE 전용: 연결 대상 Edge UUID
    int               slot               = 0;       // upstream_conns 내 자신의 인덱스
};

// ── 순수 헬퍼 (배열 + count 를 직접 받아 EdgeContext 불필요) ──────────────────

// kind 가 일치하고 connected=true 인 첫 번째 슬롯 반환 (없으면 nullptr)
inline UpstreamConn* upstream_find(UpstreamConn* conns, int count, UpstreamKind kind)
{
    for (int i = 0; i < count; i++) {
        if (conns[i].mosq && conns[i].kind == kind && conns[i].connected)
            return &conns[i];
    }
    return nullptr;
}

// kind 가 일치하는 첫 번째 슬롯 반환 (connected 무관, mosq 만 non-null 이면 됨)
inline UpstreamConn* upstream_find_any(UpstreamConn* conns, int count, UpstreamKind kind)
{
    for (int i = 0; i < count; i++) {
        if (conns[i].mosq && conns[i].kind == kind)
            return &conns[i];
    }
    return nullptr;
}

// preferred=true 이고 connected=true 인 첫 번째 슬롯 반환 (없으면 nullptr)
inline UpstreamConn* upstream_preferred(UpstreamConn* conns, int count)
{
    for (int i = 0; i < count; i++) {
        if (conns[i].mosq && conns[i].preferred && conns[i].connected)
            return &conns[i];
    }
    return nullptr;
}

// 라우팅 우선순위에 따라 첫 번째로 시도할 슬롯 반환
// 순서: preferred → CORE → BACKUP → PEER_EDGE
// 모두 미연결이면 nullptr
inline UpstreamConn* upstream_choose(UpstreamConn* conns, int count)
{
    UpstreamConn* u;

    u = upstream_preferred(conns, count);
    if (u) return u;

    u = upstream_find(conns, count, UpstreamKind::CORE);
    if (u) return u;

    u = upstream_find(conns, count, UpstreamKind::BACKUP);
    if (u) return u;

    for (int i = 0; i < count; i++) {
        if (conns[i].mosq && conns[i].kind == UpstreamKind::PEER_EDGE && conns[i].connected)
            return &conns[i];
    }
    return nullptr;
}
