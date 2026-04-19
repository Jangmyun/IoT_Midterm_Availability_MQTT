#pragma once

// edge_helpers.h
// Edge Broker 순수 로직 헬퍼 — mosquitto 의존 없이 단위 테스트 가능
//
// 포함 함수:
//   infer_msg_type        — 토픽/페이로드 문자열에서 MsgType 추론
//   infer_priority        — MsgType에서 MsgPriority 결정
//   parse_building_camera — campus/data/<event>/<building>/<camera> 토픽 파싱
//   select_relay_node     — RTT 최소 + hop_to_core 최소 기준 Relay Node 선택 (FR-08)

#include <string>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cstring>
#include <cstdio>

#include "connection_table.h"
#include "connection_table_manager.h"
#include "message.h"
#include "mqtt_json.h"

// campus/data/ 토픽 prefix (파싱 기준점)
static constexpr const char* EDGE_DATA_TOPIC_PREFIX = "campus/data/";

// ── 내부 헬퍼 ────────────────────────────────────────────────────────────────

inline std::string edge_str_tolower(const std::string& s)
{
    std::string x = s;
    std::transform(x.begin(), x.end(), x.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return x;
}

// ── 공개 API ──────────────────────────────────────────────────────────────────

// 토픽/페이로드 문자열(소문자)에서 이벤트 타입 추론
// intrusion > door_forced > motion (우선순위 순)
inline MsgType infer_msg_type(const char* topic, const std::string& payload)
{
    std::string t = edge_str_tolower(topic ? topic : "");
    std::string p = edge_str_tolower(payload);

    if (t.find("intrusion") != std::string::npos ||
        p.find("intrusion") != std::string::npos)
        return MSG_TYPE_INTRUSION;

    if (t.find("door") != std::string::npos ||
        p.find("door") != std::string::npos)
        return MSG_TYPE_DOOR_FORCED;

    return MSG_TYPE_MOTION;
}

// MsgType → MsgPriority 결정
inline MsgPriority infer_priority(MsgType type)
{
    if (type == MSG_TYPE_INTRUSION || type == MSG_TYPE_DOOR_FORCED)
        return PRIORITY_HIGH;
    if (type == MSG_TYPE_MOTION)
        return PRIORITY_MEDIUM;
    return PRIORITY_NONE;
}

// campus/data/<event_type>[/<building_id>/<camera_id>] 형식의 토픽 파싱
// prefix 이후 첫 번째 '/' 앞을 building_id, 뒤를 camera_id 로 추출
// campus/data/<event_type> 처럼 세그먼트가 하나뿐이면 building_id 에 넣고 camera_id 는 빈 문자열
inline void parse_building_camera(const char* topic,
    char* building, size_t building_len,
    char* camera,   size_t camera_len)
{
    if (building_len > 0) building[0] = '\0';
    if (camera_len  > 0) camera[0]   = '\0';
    if (!topic) return;

    size_t prefix_len = std::strlen(EDGE_DATA_TOPIC_PREFIX);
    if (std::strncmp(topic, EDGE_DATA_TOPIC_PREFIX, prefix_len) != 0)
        return;

    const char* rest  = topic + prefix_len;
    const char* slash = std::strchr(rest, '/');

    if (!slash)
    {
        std::snprintf(building, building_len, "%s", rest);
        return;
    }

    size_t part_len = (size_t)(slash - rest);
    std::snprintf(building, building_len, "%.*s", (int)part_len, rest);
    std::snprintf(camera,   camera_len,   "%s",   slash + 1);
}

// nested publisher MqttMessage JSON에서 "원래 소속 edge"를 나타내는 route.original_node를 추출한다.
// publisher 기본값은 route.original_node == source.id 이므로, 두 값이 다를 때만 origin edge override로 간주한다.
inline bool extract_nested_event_origin_node(const std::string& payload,
                                             char* out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;

    out[0] = '\0';
    if (payload.empty())
        return false;

    MqttMessage nested = {};
    if (!mqtt_message_from_json(payload, nested))
        return false;

    if (nested.route.original_node[0] == '\0')
        return false;

    if (nested.source.id[0] != '\0' &&
        std::strncmp(nested.route.original_node, nested.source.id, UUID_LEN) == 0)
        return false;

    std::snprintf(out, out_len, "%s", nested.route.original_node);
    return true;
}

// RTT + hop_to_core 기반 최적 Relay Node UUID 반환 (FR-08, 시나리오 5.6)
//   1차 기준: RTT(ms) 최소
//   2차 기준: hop_to_core 최소 (RTT 동점 시)
//   RTT가 아직 측정되지 않은 노드(FLT_MAX)는 후보 제외
//   후보 없으면 빈 문자열 반환
inline std::string select_relay_node(ConnectionTableManager& ct_manager,
                                     const char* edge_id)
{
    ConnectionTable ct = ct_manager.snapshot();
    std::string best_id;
    float best_rtt = FLT_MAX;
    int   best_hop = INT_MAX;

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& n = ct.nodes[i];
        if (n.role != NODE_ROLE_NODE)                              continue;
        if (n.status != NODE_STATUS_ONLINE)                        continue;
        if (std::strncmp(n.id, edge_id, UUID_LEN) == 0)           continue;

        float rtt = FLT_MAX;
        for (int j = 0; j < ct.link_count; j++)
        {
            if (std::strncmp(ct.links[j].from_id, edge_id, UUID_LEN) == 0
                && std::strncmp(ct.links[j].to_id, n.id, UUID_LEN) == 0)
            {
                rtt = ct.links[j].rtt_ms;
                break;
            }
        }

        if (rtt == FLT_MAX) continue;

        if (rtt < best_rtt || (rtt == best_rtt && n.hop_to_core < best_hop))
        {
            best_rtt = rtt;
            best_hop = n.hop_to_core;
            best_id  = n.id;
        }
    }
    return best_id;
}

// campus/will/core/<id> 수신 시 실제 failover가 필요한지 판정
// 현재 active_core_id 와 일치할 때만 backup 우선 모드로 전환한다.
// CT 초기 동기화 전(active_core_id 미확정)에는 보수적으로 true를 반환한다.
inline bool should_failover_on_core_will(const ConnectionTableManager& ct_manager,
                                         const char* failed_core_id)
{
    if (!failed_core_id || failed_core_id[0] == '\0')
        return false;

    ConnectionTable ct = ct_manager.snapshot();
    if (ct.active_core_id[0] == '\0')
        return true;

    return std::strncmp(ct.active_core_id, failed_core_id, UUID_LEN) == 0;
}

inline bool is_backup_core_will(const ConnectionTableManager& ct_manager,
                                const char* failed_core_id)
{
    if (!failed_core_id || failed_core_id[0] == '\0')
        return false;

    ConnectionTable ct = ct_manager.snapshot();
    if (ct.backup_core_id[0] == '\0')
        return false;

    return std::strncmp(ct.backup_core_id, failed_core_id, UUID_LEN) == 0;
}
