#pragma once

// publisher_helpers.h
// Publisher Event Simulator 순수 로직 헬퍼 — mosquitto 의존 없이 단위 테스트 가능
//
// 포함 함수:
//   build_event_topic      — campus/data/<type>/<building>/<camera> 토픽 생성
//   set_now_utc            — ISO 8601 UTC 타임스탬프 채우기
//   build_event_message    — MqttMessage 구성
//   next_event_type        — event_mask 기반 round-robin 이벤트 타입 선택
//   msg_type_to_topic_segment — MsgType → 토픽 세그먼트 문자열
//   mark_message_as_dup    — 중복 메시지 마킹 (delivery.dup = true)
//   rate_to_sleep_us       — rate_hz → usleep 간격 계산
//   parse_publisher_args   — CLI args → PublisherConfig 파싱

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

#include "message.h"
#include "uuid.h"
#include "edge_helpers.h"   // infer_priority() 재사용

// ── PublisherConfig ───────────────────────────────────────────────────────────

struct PublisherConfig {
    char broker_host[64];              // default: "localhost"
    int  broker_port;                  // default: 1883
    char publisher_id[UUID_LEN];       // default: auto-generated UUID
    char building_id[BUILDING_ID_LEN]; // default: "building-a"
    char camera_id[CAMERA_ID_LEN];     // default: "cam-01"
    int  count;                        // 전송 횟수 (0=무제한, default: 10)
    int  rate_hz;                      // 초당 이벤트 수 (default: 1)
    int  qos;                          // MQTT QoS 0/1/2 (default: 1)
    bool dup_inject;                   // 중복 재전송 여부
    int  dup_count;                    // 중복 횟수 (default: 1)
    bool burst_mode;                   // rate 무시, 최대 속도 전송
    bool register_edge;                // 루프 전 STATUS 등록 메시지 전송
    int  event_mask;                   // bit0=MOTION, bit1=DOOR_FORCED, bit2=INTRUSION (default: 7)
    bool multi_pub;                    // 이벤트마다 새 UUID (다수 Edge 시뮬레이션)
    char description[DESCRIPTION_LEN]; // payload.description 태그 (default: "sim-pub")
    bool verbose;                      // 전송 메시지 출력
};

// event_mask 비트 상수
static constexpr int PUB_MASK_MOTION    = 1 << 0;
static constexpr int PUB_MASK_DOOR      = 1 << 1;
static constexpr int PUB_MASK_INTRUSION = 1 << 2;

// ── build_event_topic ─────────────────────────────────────────────────────────

// "campus/data/<type_str>/<building>/<camera>" 토픽 생성
// type_str, building, camera 중 하나라도 비어있으면 false 반환
inline bool build_event_topic(const char* type_str, const char* building,
                               const char* camera, char* out, size_t len)
{
    if (!type_str || type_str[0] == '\0') return false;
    if (!building  || building[0]  == '\0') return false;
    if (!camera    || camera[0]    == '\0') return false;
    if (!out || len == 0) return false;

    int n = std::snprintf(out, len, "campus/data/%s/%s/%s", type_str, building, camera);
    return n > 0 && (size_t)n < len;
}

// ── set_now_utc ───────────────────────────────────────────────────────────────

// ISO 8601 UTC 타임스탬프 채우기: "2026-04-18T12:00:00Z"
inline bool set_now_utc(char* out, size_t len)
{
    if (!out || len < 21) return false;
    std::time_t now = std::time(nullptr);
    std::tm* utc = std::gmtime(&now);
    if (!utc) return false;
    std::strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", utc);
    return true;
}

// ── build_event_message ───────────────────────────────────────────────────────

// MqttMessage 구성 (infer_priority 재사용)
// publisher_id 가 비어있으면 false 반환
inline bool build_event_message(const char* publisher_id, MsgType type,
                                 const char* building, const char* camera,
                                 const char* desc, int qos, MqttMessage* out)
{
    if (!publisher_id || publisher_id[0] == '\0') return false;
    if (!out) return false;

    *out = {};  // zero-initialize

    uuid_generate(out->msg_id);
    out->type     = type;
    out->priority = infer_priority(type);
    set_now_utc(out->timestamp, sizeof(out->timestamp));

    out->source.role = NODE_ROLE_NODE;
    std::strncpy(out->source.id, publisher_id, UUID_LEN - 1);

    out->target.role = NODE_ROLE_CORE;
    // target.id 비워둠 (Core 방향)

    std::strncpy(out->route.original_node, publisher_id, UUID_LEN - 1);
    out->route.hop_count = 0;
    out->route.ttl       = 8;

    out->delivery.qos    = qos;
    out->delivery.dup    = false;
    out->delivery.retain = false;

    if (building) std::strncpy(out->payload.building_id, building, BUILDING_ID_LEN - 1);
    if (camera)   std::strncpy(out->payload.camera_id,   camera,   CAMERA_ID_LEN   - 1);
    if (desc)     std::strncpy(out->payload.description,  desc,     DESCRIPTION_LEN - 1);

    return true;
}

// ── next_event_type ───────────────────────────────────────────────────────────

// event_mask 기반 round-robin 이벤트 타입 선택
// state: 호출 측에서 관리 (0으로 초기화)
// event_mask == 0 이면 MOTION fallback
inline MsgType next_event_type(int event_mask, int* state)
{
    if (event_mask == 0 || !state) return MSG_TYPE_MOTION;

    // 활성화된 타입 목록 구성 (고정 순서: MOTION, DOOR_FORCED, INTRUSION)
    MsgType enabled[3];
    int count = 0;
    if (event_mask & PUB_MASK_MOTION)    enabled[count++] = MSG_TYPE_MOTION;
    if (event_mask & PUB_MASK_DOOR)      enabled[count++] = MSG_TYPE_DOOR_FORCED;
    if (event_mask & PUB_MASK_INTRUSION) enabled[count++] = MSG_TYPE_INTRUSION;

    if (count == 0) return MSG_TYPE_MOTION;

    MsgType result = enabled[*state % count];
    (*state)++;
    return result;
}

// ── msg_type_to_topic_segment ─────────────────────────────────────────────────

// MsgType → topic segment 문자열 ("motion" / "door" / "intrusion")
inline const char* msg_type_to_topic_segment(MsgType t)
{
    switch (t) {
        case MSG_TYPE_DOOR_FORCED: return "door";
        case MSG_TYPE_INTRUSION:   return "intrusion";
        default:                   return "motion";
    }
}

// ── mark_message_as_dup ───────────────────────────────────────────────────────

// 중복 메시지 마킹: delivery.dup = true
inline void mark_message_as_dup(MqttMessage* msg)
{
    if (msg) msg->delivery.dup = true;
}

// ── rate_to_sleep_us ──────────────────────────────────────────────────────────

// rate_hz → usleep 간격 (microseconds)
// rate_hz <= 0 이면 0 반환 (슬립 없음)
inline long rate_to_sleep_us(int rate_hz)
{
    if (rate_hz <= 0) return 0L;
    return (long)(1000000L / rate_hz);
}

// ── Failover 로직 (Phase 7): CT 기반 대체 브로커 자동 선택 ───────────────────

// select_fallback_broker() 반환값
struct FallbackBroker {
    char id[UUID_LEN];
    char ip[IP_LEN];
    uint16_t port;
    bool found;          // 후보가 있으면 true
};

inline void fill_fallback_broker(const NodeEntry& node, FallbackBroker* out)
{
    if (!out) return;
    *out = {};
    std::strncpy(out->id, node.id, UUID_LEN - 1);
    out->id[UUID_LEN - 1] = '\0';
    std::strncpy(out->ip, node.ip, IP_LEN - 1);
    out->ip[IP_LEN - 1] = '\0';
    out->port  = node.port;
    out->found = true;
}

inline bool is_online_broker_node(const NodeEntry& node)
{
    return node.status == NODE_STATUS_ONLINE &&
           node.ip[0] != '\0' &&
           node.port != 0;
}

inline float lookup_link_rtt(const ConnectionTable& ct,
                             const char* from_id,
                             const char* to_id)
{
    if (!from_id || from_id[0] == '\0' || !to_id || to_id[0] == '\0')
        return FLT_MAX;

    float best_rtt = FLT_MAX;
    for (int i = 0; i < ct.link_count; i++)
    {
        const bool same_direction =
            std::strncmp(ct.links[i].from_id, from_id, UUID_LEN) == 0 &&
            std::strncmp(ct.links[i].to_id, to_id, UUID_LEN) == 0;
        const bool reverse_direction =
            std::strncmp(ct.links[i].from_id, to_id, UUID_LEN) == 0 &&
            std::strncmp(ct.links[i].to_id, from_id, UUID_LEN) == 0;
        if (!same_direction && !reverse_direction)
            continue;

        if (ct.links[i].rtt_ms < best_rtt)
            best_rtt = ct.links[i].rtt_ms;
    }

    return best_rtt;
}

inline const NodeEntry* find_online_core_by_id(const ConnectionTable& ct,
                                               const char* core_id)
{
    if (!core_id || core_id[0] == '\0')
        return nullptr;

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& node = ct.nodes[i];
        if (node.role != NODE_ROLE_CORE)
            continue;
        if (!is_online_broker_node(node))
            continue;
        if (std::strncmp(node.id, core_id, UUID_LEN) == 0)
            return &ct.nodes[i];
    }

    return nullptr;
}

inline const NodeEntry* select_best_node_by_rtt_and_hop(const ConnectionTable& ct,
                                                        NodeRole role,
                                                        const char* primary_edge_id,
                                                        const char* exclude_a = nullptr,
                                                        const char* exclude_b = nullptr)
{
    const NodeEntry* best = nullptr;
    float best_rtt = FLT_MAX;
    int best_hop = INT_MAX;

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& node = ct.nodes[i];
        if (node.role != role)
            continue;
        if (!is_online_broker_node(node))
            continue;
        if (exclude_a && exclude_a[0] != '\0' &&
            std::strncmp(node.id, exclude_a, UUID_LEN) == 0)
            continue;
        if (exclude_b && exclude_b[0] != '\0' &&
            std::strncmp(node.id, exclude_b, UUID_LEN) == 0)
            continue;

        const float rtt = lookup_link_rtt(ct, primary_edge_id, node.id);
        if (!best ||
            rtt < best_rtt ||
            (rtt == best_rtt && node.hop_to_core < best_hop))
        {
            best = &ct.nodes[i];
            best_rtt = rtt;
            best_hop = node.hop_to_core;
        }
    }

    return best;
}

// Core fallback 후보를 선택한다.
// active core를 최우선으로 하고, 없으면 backup core, 둘 다 없으면 ONLINE core 중 RTT/hop 기준으로 선택.
inline FallbackBroker select_preferred_core_broker(const ConnectionTable& ct,
                                                   const char* primary_edge_id)
{
    FallbackBroker result = {};

    const NodeEntry* active_core = find_online_core_by_id(ct, ct.active_core_id);
    if (active_core) {
        fill_fallback_broker(*active_core, &result);
        return result;
    }

    const NodeEntry* backup_core = find_online_core_by_id(ct, ct.backup_core_id);
    if (backup_core) {
        fill_fallback_broker(*backup_core, &result);
        return result;
    }

    const NodeEntry* best_other_core = select_best_node_by_rtt_and_hop(
        ct, NODE_ROLE_CORE, primary_edge_id, ct.active_core_id, ct.backup_core_id);
    if (best_other_core) {
        fill_fallback_broker(*best_other_core, &result);
    }

    return result;
}

// CT에서 ONLINE 노드 중 primary_edge_id를 제외하고 최적 대체 브로커 선택 (FR-08 확장)
//   1차 기준: 다른 Edge(NODE_ROLE_NODE) 중 RTT 최소, 동점 시 hop_to_core 최소
//   Edge 후보가 없으면 active core → backup core → 기타 ONLINE core 순으로 선택
//   모두 OFFLINE이면 found=false
inline FallbackBroker select_fallback_broker(const ConnectionTable& ct,
                                             const char* primary_edge_id)
{
    FallbackBroker result = {};

    const NodeEntry* best_edge = select_best_node_by_rtt_and_hop(
        ct, NODE_ROLE_NODE, primary_edge_id, primary_edge_id, nullptr);
    if (best_edge) {
        fill_fallback_broker(*best_edge, &result);
        return result;
    }

    return select_preferred_core_broker(ct, primary_edge_id);
}

// primary edge가 CT에서 ONLINE 상태인지 확인 — ONLINE이면 원래 Edge로 복귀
inline bool should_return_to_primary(const ConnectionTable& ct,
                                      const char* primary_edge_id)
{
    if (!primary_edge_id || primary_edge_id[0] == '\0')
        return false;

    for (int i = 0; i < ct.node_count; i++)
    {
        if (std::strncmp(ct.nodes[i].id, primary_edge_id, UUID_LEN) == 0)
        {
            return ct.nodes[i].status == NODE_STATUS_ONLINE;
        }
    }
    return false;  // CT에 없으면 복귀 불가
}

// ── parse_publisher_args ──────────────────────────────────────────────────────

// argv 에서 "--events" 인자 파싱: "motion,door,intrusion" → event_mask
inline int parse_event_mask(const char* arg)
{
    if (!arg) return 0;
    int mask = 0;
    std::string s(arg);
    // 쉼표로 분리
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        std::string tok = s.substr(pos, comma - pos);
        if (tok == "motion")    mask |= PUB_MASK_MOTION;
        if (tok == "door")      mask |= PUB_MASK_DOOR;
        if (tok == "intrusion") mask |= PUB_MASK_INTRUSION;
        pos = comma + 1;
    }
    return mask;
}

// CLI args → PublisherConfig
// 파싱 실패 시 stderr에 사용법 출력 후 false 반환
inline bool parse_publisher_args(int argc, char* argv[], PublisherConfig* out)
{
    if (!out) return false;

    // 기본값 설정
    *out = {};
    std::strncpy(out->broker_host, "localhost", sizeof(out->broker_host) - 1);
    out->broker_port = 1883;
    uuid_generate(out->publisher_id);
    std::strncpy(out->building_id,  "building-a", BUILDING_ID_LEN  - 1);
    std::strncpy(out->camera_id,    "cam-01",     CAMERA_ID_LEN    - 1);
    std::strncpy(out->description,  "sim-pub",    DESCRIPTION_LEN  - 1);
    out->count      = 10;
    out->rate_hz    = 1;
    out->qos        = 1;
    out->dup_inject = false;
    out->dup_count  = 1;
    out->burst_mode = false;
    out->register_edge = false;
    out->event_mask = PUB_MASK_MOTION | PUB_MASK_DOOR | PUB_MASK_INTRUSION;
    out->multi_pub  = false;
    out->verbose    = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            std::strncpy(out->broker_host, argv[++i], sizeof(out->broker_host) - 1);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            int p = std::atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                std::fprintf(stderr, "오류: 유효하지 않은 포트 번호: %d\n", p);
                return false;
            }
            out->broker_port = p;
        } else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            std::strncpy(out->publisher_id, argv[++i], UUID_LEN - 1);
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            out->count = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            out->rate_hz = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--qos") == 0 && i + 1 < argc) {
            out->qos = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--building") == 0 && i + 1 < argc) {
            std::strncpy(out->building_id, argv[++i], BUILDING_ID_LEN - 1);
        } else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            std::strncpy(out->camera_id, argv[++i], CAMERA_ID_LEN - 1);
        } else if (std::strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
            std::strncpy(out->description, argv[++i], DESCRIPTION_LEN - 1);
        } else if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
            out->event_mask = parse_event_mask(argv[++i]);
        } else if (std::strcmp(argv[i], "--burst") == 0) {
            out->burst_mode = true;
        } else if (std::strcmp(argv[i], "--dup") == 0) {
            out->dup_inject = true;
            // 다음 인자가 숫자면 dup_count로 사용
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int n = std::atoi(argv[++i]);
                if (n > 0) out->dup_count = n;
            }
        } else if (std::strcmp(argv[i], "--register") == 0) {
            out->register_edge = true;
        } else if (std::strcmp(argv[i], "--multi-pub") == 0) {
            out->multi_pub = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            out->verbose = true;
        }
    }

    return true;
}
