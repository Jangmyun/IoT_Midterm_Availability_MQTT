#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <deque>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"
#include "core_helpers.h"
#include "edge_helpers.h"
#include "edge_upstream.h"

// Global State =====================================================
static volatile bool g_running = true;

struct QueuedEvent
{
    char topic[128];
    MqttMessage msg;
};

// UpstreamCallbackCtx — mosquitto 콜백 userdata
// EdgeContext 는 아래에서 정의되므로 forward declaration 사용
struct EdgeContext;

struct UpstreamCallbackCtx
{
    EdgeContext* ctx;
    int          slot;  // upstream_conns 내 인덱스
};

struct EdgeContext
{
    char edge_id[UUID_LEN];
    char node_ip[IP_LEN];
    uint16_t node_port;
    ConnectionTableManager* ct_manager;

    // upstream 연결 배열 (mosq_core / mosq_backup 대체)
    UpstreamConn           upstream_conns[MAX_UPSTREAM];
    int                    upstream_count = 0;
    std::mutex             upstream_mutex;   // PEER_EDGE 슬롯 동적 추가 보호
    UpstreamCallbackCtx*   upstream_cb[MAX_UPSTREAM] = {};  // heap-allocated, owned

    // 로컬 CCTV 이벤트 수집용 (변경 없음)
    struct mosquitto* mosq_local;

    // store-and-forward queue
    std::deque<QueuedEvent> store_queue;
    std::mutex queue_mutex;
    std::mutex flush_mutex;

    int last_ct_version = 0;

    // 현재 연결 중인 Active Core 주소
    char active_core_ip[IP_LEN];
    int  active_core_port;

    // RTT 측정: target_node_id → ping 발송 시각 (FR-08)
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> ping_send_times;
    std::mutex ping_mutex;

    // 현재 선택된 최적 Relay Node UUID (없으면 빈 문자열) (FR-08)
    char relay_node_id[UUID_LEN];
};

// ── 인라인 접근 헬퍼 ──────────────────────────────────────────────────────────

static UpstreamConn* find_upstream(EdgeContext* ctx, UpstreamKind kind)
{
    return upstream_find(ctx->upstream_conns, ctx->upstream_count, kind);
}

static UpstreamConn* find_upstream_any(EdgeContext* ctx, UpstreamKind kind)
{
    return upstream_find_any(ctx->upstream_conns, ctx->upstream_count, kind);
}

static UpstreamConn* preferred_upstream_conn(EdgeContext* ctx)
{
    return upstream_preferred(ctx->upstream_conns, ctx->upstream_count);
}

static void handle_signal(int) { g_running = false; }

// core 방향 outbound IP 감지 (실제 패킷 전송 없음)
static bool get_outbound_ip(const char* dest_ip, int dest_port, char* out, size_t len)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)dest_port);
    dest.sin_addr.s_addr = inet_addr(dest_ip);

    if (connect(sock, (sockaddr*)&dest, sizeof(dest)) < 0)
    {
        close(sock);
        return false;
    }

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    getsockname(sock, (sockaddr*)&local, &local_len);
    close(sock);

    inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)len);
    return true;
}

static void set_now_utc(char* out, size_t len)
{
    std::time_t now = std::time(nullptr);
    std::tm* utc = std::gmtime(&now);
    if (!utc)
    {
        if (len > 0)
            out[0] = '\0';
        return;
    }
    std::strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", utc);
}

static std::string build_edge_lwt_json(EdgeContext* ctx)
{
    MqttMessage lwt = {};
    uuid_generate(lwt.msg_id);
    lwt.type = MSG_TYPE_LWT_NODE;
    lwt.source.role = NODE_ROLE_NODE;
    std::strncpy(lwt.source.id, ctx->edge_id, UUID_LEN - 1);
    lwt.source.id[UUID_LEN - 1] = '\0';
    lwt.delivery = { 1, false, false };
    return mqtt_message_to_json(lwt);
}

static void publish_edge_down_notice(struct mosquitto* mosq, EdgeContext* ctx)
{
    if (!mosq || !ctx) {
        return;
    }

    char topic[128];
    std::snprintf(topic, sizeof(topic), "%s%s", TOPIC_LWT_NODE_PREFIX, ctx->edge_id);
    std::string lwt_json = build_edge_lwt_json(ctx);
    mosquitto_publish(mosq, nullptr, topic,
        (int)lwt_json.size(), lwt_json.c_str(), 1, false);
}

// core / backup / peer 공통 등록 함수
static void publish_edge_status(struct mosquitto* mosq, EdgeContext* ctx, const char* label)
{
    MqttMessage reg = {};
    uuid_generate(reg.msg_id);
    reg.type = MSG_TYPE_STATUS;
    reg.source.role = NODE_ROLE_NODE;
    std::strncpy(reg.source.id, ctx->edge_id, UUID_LEN - 1);
    reg.source.id[UUID_LEN - 1] = '\0';

    reg.target.role = NODE_ROLE_CORE;
    reg.target.id[0] = '\0';

    reg.delivery = { 1, false, false };
    std::snprintf(reg.payload.description, DESCRIPTION_LEN,
        "%s:%u", ctx->node_ip, ctx->node_port);

    char status_topic[128];
    std::snprintf(status_topic, sizeof(status_topic), "%s%s",
        TOPIC_STATUS_PREFIX, ctx->edge_id);

    std::string json = mqtt_message_to_json(reg);
    mosquitto_publish(mosq, nullptr, status_topic,
        (int)json.size(), json.c_str(), 1, false);

    std::printf("[edge] registered to %s: id=%s  ip=%s:%u\n",
        label, ctx->edge_id, ctx->node_ip, ctx->node_port);
}

static void build_event_message(EdgeContext* ctx, const char* topic,
    const std::string& payload, MqttMessage* out_msg)
{
    MqttMessage msg = {};
    uuid_generate(msg.msg_id);
    set_now_utc(msg.timestamp, sizeof(msg.timestamp));

    msg.type = infer_msg_type(topic, payload);
    msg.priority = infer_priority(msg.type);

    msg.source.role = NODE_ROLE_NODE;
    std::strncpy(msg.source.id, ctx->edge_id, UUID_LEN - 1);
    msg.source.id[UUID_LEN - 1] = '\0';

    msg.target.role = NODE_ROLE_CORE;
    msg.target.id[0] = '\0';

    std::strncpy(msg.route.original_node, ctx->edge_id, UUID_LEN - 1);
    msg.route.original_node[UUID_LEN - 1] = '\0';

    char origin_edge_id[UUID_LEN] = {};
    if (extract_nested_event_origin_node(payload, origin_edge_id, sizeof(origin_edge_id)))
    {
        std::strncpy(msg.route.original_node, origin_edge_id, UUID_LEN - 1);
        msg.route.original_node[UUID_LEN - 1] = '\0';
    }

    std::strncpy(msg.route.prev_hop, ctx->edge_id, UUID_LEN - 1);
    msg.route.prev_hop[UUID_LEN - 1] = '\0';

    msg.route.next_hop[0] = '\0';
    msg.route.hop_count = 0;
    msg.route.ttl = 8;

    msg.delivery = { 1, false, false };

    parse_building_camera(topic,
        msg.payload.building_id, sizeof(msg.payload.building_id),
        msg.payload.camera_id, sizeof(msg.payload.camera_id));

    std::strncpy(msg.payload.description, payload.c_str(), DESCRIPTION_LEN - 1);
    msg.payload.description[DESCRIPTION_LEN - 1] = '\0';

    *out_msg = msg;
}

static bool publish_to_upstream(struct mosquitto* mosq, const char* label,
    const char* topic, const MqttMessage& msg)
{
    if (!mosq)
        return false;

    std::string json = mqtt_message_to_json(msg);
    int rc = mosquitto_publish(mosq, nullptr, topic,
        (int)json.size(), json.c_str(),
        msg.delivery.qos, msg.delivery.retain);

    if (rc == MOSQ_ERR_SUCCESS)
    {
        std::printf("[edge] forwarded to %s\n", label);
        std::printf("  topic   : %s\n", topic);
        std::printf("  msg_id  : %s\n", msg.msg_id);
        return true;
    }

    std::fprintf(stderr, "[edge] publish to %s failed: %s\n",
        label, mosquitto_strerror(rc));
    return false;
}

static bool forward_message_upstream(EdgeContext* ctx, const char* topic, const MqttMessage& msg)
{
    // 1. preferred 슬롯 (페일오버 우선 경로)
    if (UpstreamConn* pref = preferred_upstream_conn(ctx))
    {
        const char* label = (pref->kind == UpstreamKind::BACKUP) ? "backup core" : "peer edge";
        if (publish_to_upstream(pref->mosq, label, topic, msg))
            return true;
    }

    // 2. CORE 슬롯
    if (UpstreamConn* core = find_upstream(ctx, UpstreamKind::CORE))
    {
        if (publish_to_upstream(core->mosq, "core", topic, msg))
            return true;
    }

    // 3. BACKUP 슬롯
    if (UpstreamConn* bk = find_upstream(ctx, UpstreamKind::BACKUP))
    {
        if (publish_to_upstream(bk->mosq, "backup core", topic, msg))
            return true;
    }

    // 4. PEER_EDGE 슬롯들
    for (int i = 0; i < ctx->upstream_count; i++)
    {
        UpstreamConn& up = ctx->upstream_conns[i];
        if (up.mosq && up.kind == UpstreamKind::PEER_EDGE && up.connected)
        {
            if (publish_to_upstream(up.mosq, "peer edge", topic, msg))
                return true;
        }
    }

    // 5. campus/relay/<relay_node_id> 폴백 (CORE mosq 존재 시, 기존 동작 유지)
    if (ctx->relay_node_id[0] != '\0')
    {
        UpstreamConn* core = find_upstream_any(ctx, UpstreamKind::CORE);
        if (core && core->mosq)
        {
            char relay_topic[128];
            std::snprintf(relay_topic, sizeof(relay_topic), "campus/relay/%s", ctx->relay_node_id);
            if (publish_to_upstream(core->mosq, "relay node", relay_topic, msg))
                return true;
        }
    }

    return false;
}

static bool has_pending_queue(EdgeContext* ctx)
{
    std::lock_guard<std::mutex> lock(ctx->queue_mutex);
    return !ctx->store_queue.empty();
}

static void queue_event(EdgeContext* ctx, const char* topic, const MqttMessage& msg)
{
    QueuedEvent item = {};
    std::strncpy(item.topic, topic, sizeof(item.topic) - 1);
    item.topic[sizeof(item.topic) - 1] = '\0';
    item.msg = msg;

    std::lock_guard<std::mutex> lock(ctx->queue_mutex);
    ctx->store_queue.push_back(item);

    std::printf("[edge] queued event for later delivery\n");
    std::printf("  topic      : %s\n", item.topic);
    std::printf("  msg_id     : %s\n", item.msg.msg_id);
    std::printf("  queue_size : %zu\n", ctx->store_queue.size());
}

static void flush_store_queue(EdgeContext* ctx)
{
    std::lock_guard<std::mutex> flush_lock(ctx->flush_mutex);

    while (true)
    {
        QueuedEvent item = {};
        {
            std::lock_guard<std::mutex> queue_lock(ctx->queue_mutex);
            if (ctx->store_queue.empty())
                return;

            item = ctx->store_queue.front();
        }

        if (!forward_message_upstream(ctx, item.topic, item.msg))
        {
            return;
        }

        {
            std::lock_guard<std::mutex> queue_lock(ctx->queue_mutex);
            if (!ctx->store_queue.empty() &&
                std::strncmp(ctx->store_queue.front().msg.msg_id, item.msg.msg_id, UUID_LEN) == 0)
            {
                ctx->store_queue.pop_front();
                std::printf("[edge] flushed one queued event\n");
                std::printf("  msg_id     : %s\n", item.msg.msg_id);
                std::printf("  queue_size : %zu\n", ctx->store_queue.size());
            }
        }
    }
}

static void handle_local_event_delivery(EdgeContext* ctx, const char* topic, const std::string& payload)
{
    MqttMessage event_msg = {};
    build_event_message(ctx, topic, payload, &event_msg);

    if (has_pending_queue(ctx))
    {
        flush_store_queue(ctx);

        if (has_pending_queue(ctx))
        {
            queue_event(ctx, topic, event_msg);
            return;
        }
    }

    if (forward_message_upstream(ctx, topic, event_msg))
    {
        return;
    }

    queue_event(ctx, topic, event_msg);
}

// CT 수신 후 ONLINE NODE에 Ping 발송 → RTT 측정 시작 (FR-08)
static void send_pings_to_nodes(EdgeContext* ctx)
{
    UpstreamConn* core = find_upstream(ctx, UpstreamKind::CORE);
    if (!core)
        return;

    ConnectionTable ct = ctx->ct_manager->snapshot();
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& n = ct.nodes[i];
        if (n.role != NODE_ROLE_NODE)              continue;
        if (n.status != NODE_STATUS_ONLINE)        continue;
        if (std::strncmp(n.id, ctx->edge_id, UUID_LEN) == 0) continue;

        {
            std::lock_guard<std::mutex> lock(ctx->ping_mutex);
            ctx->ping_send_times[n.id] = now;
        }

        MqttMessage ping = {};
        uuid_generate(ping.msg_id);
        ping.type = MSG_TYPE_PING_REQUEST;
        ping.source.role = NODE_ROLE_NODE;
        std::strncpy(ping.source.id, ctx->edge_id, UUID_LEN - 1);
        ping.source.id[UUID_LEN - 1] = '\0';
        ping.target.role = NODE_ROLE_NODE;
        std::strncpy(ping.target.id, n.id, UUID_LEN - 1);
        ping.target.id[UUID_LEN - 1] = '\0';
        ping.delivery = { 0, false, false };

        char topic[128];
        std::snprintf(topic, sizeof(topic), "%s%s", TOPIC_PING_PREFIX, n.id);

        std::string json = mqtt_message_to_json(ping);
        mosquitto_publish(core->mosq, nullptr, topic,
            (int)json.size(), json.c_str(), 0, false);

        // 로컬 브로커에도 발행 → peer-only edge 가 로컬 브로커를 통해 ping 수신 가능
        if (ctx->mosq_local)
            mosquitto_publish(ctx->mosq_local, nullptr, topic,
                (int)json.size(), json.c_str(), 0, false);

        std::printf("[edge] ping sent to %s\n", n.id);
    }
}

// Forward declaration (pong 핸들러에서 호출)
static void maybe_establish_peer_conn(EdgeContext* ctx, const std::string& peer_node_id);

// Ping/Pong 공통 처리 헬퍼 (CORE·PEER_EDGE upstream 및 local broker 모두 사용)
static void process_ping(struct mosquitto* reply_mosq, EdgeContext* ctx,
    const struct mosquitto_message* msg)
{
    MqttMessage ping;
    std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
    if (!mqtt_message_from_json(json, ping))
        return;

    MqttMessage pong = {};
    uuid_generate(pong.msg_id);
    pong.type = MSG_TYPE_PING_RESPONSE;
    pong.source.role = NODE_ROLE_NODE;
    std::strncpy(pong.source.id, ctx->edge_id, UUID_LEN - 1);
    pong.source.id[UUID_LEN - 1] = '\0';
    pong.target.role = NODE_ROLE_NODE;
    std::strncpy(pong.target.id, ping.source.id, UUID_LEN - 1);
    pong.target.id[UUID_LEN - 1] = '\0';
    pong.delivery = { 0, false, false };

    char pong_topic[128];
    std::snprintf(pong_topic, sizeof(pong_topic), "%s%s",
        TOPIC_PONG_PREFIX, ping.source.id);

    std::string pong_json = mqtt_message_to_json(pong);
    mosquitto_publish(reply_mosq, nullptr, pong_topic,
        (int)pong_json.size(), pong_json.c_str(), 0, false);
}

static void process_pong(EdgeContext* ctx, const struct mosquitto_message* msg)
{
    MqttMessage pong;
    std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
    if (!mqtt_message_from_json(json, pong))
        return;

    if (std::strncmp(pong.target.id, ctx->edge_id, UUID_LEN) != 0)
        return;

    auto recv_time = std::chrono::steady_clock::now();
    float rtt_ms = 0.0f;
    {
        std::lock_guard<std::mutex> lock(ctx->ping_mutex);
        auto it = ctx->ping_send_times.find(pong.source.id);
        if (it == ctx->ping_send_times.end())
            return;
        rtt_ms = std::chrono::duration<float, std::milli>(recv_time - it->second).count();
        ctx->ping_send_times.erase(it);
    }

    std::printf("[edge] pong from %s: RTT=%.2fms\n", pong.source.id, rtt_ms);

    LinkEntry link = {};
    std::strncpy(link.from_id, ctx->edge_id, UUID_LEN - 1);
    std::strncpy(link.to_id, pong.source.id, UUID_LEN - 1);
    link.rtt_ms = rtt_ms;
    ctx->ct_manager->addLink(link);

    // Core CT에 peer 링크 RTT 보고 → 클라이언트 그래프 peer-link 표시
    {
        UpstreamConn* core_up = find_upstream(ctx, UpstreamKind::CORE);
        if (!core_up) core_up = find_upstream(ctx, UpstreamKind::BACKUP);
        if (core_up && core_up->mosq)
        {
            char rtt_topic[256];
            std::snprintf(rtt_topic, sizeof(rtt_topic),
                "%s%s/%s", TOPIC_RTT_PREFIX, ctx->edge_id, pong.source.id);
            char rtt_payload[32];
            std::snprintf(rtt_payload, sizeof(rtt_payload), "%.2f", rtt_ms);
            mosquitto_publish(core_up->mosq, nullptr, rtt_topic,
                (int)std::strlen(rtt_payload), rtt_payload, 1, false);
        }
    }

    std::string best = select_relay_node(*ctx->ct_manager, ctx->edge_id);
    if (!best.empty())
    {
        std::strncpy(ctx->relay_node_id, best.c_str(), UUID_LEN - 1);
        ctx->relay_node_id[UUID_LEN - 1] = '\0';
        std::printf("[edge] relay node selected: %s\n", ctx->relay_node_id);
        maybe_establish_peer_conn(ctx, best);
    }
}

// Callbacks =====================================================

static void on_connect_upstream(struct mosquitto* mosq, void* userdata, int rc)
{
    auto* cb = static_cast<UpstreamCallbackCtx*>(userdata);
    auto* ctx = cb->ctx;
    UpstreamConn& up = ctx->upstream_conns[cb->slot];

    if (rc != 0)
    {
        up.connected = false;
        std::fprintf(stderr, "[edge] upstream[%d] connect failed (rc=%d)\n", cb->slot, rc);
        return;
    }

    up.connected = true;

    switch (up.kind)
    {
    case UpstreamKind::CORE:
        ctx->last_ct_version = 0;  // 재연결 시 CT 버전 리셋
        // 모든 슬롯의 preferred 초기화 (core 가 다시 primary)
        for (int i = 0; i < ctx->upstream_count; i++)
            ctx->upstream_conns[i].preferred = false;
        std::printf("[edge] connected to core\n");
        std::printf("[edge] core is primary again\n");
        {
            char ping_topic[128];
            std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
            char pong_topic[128];
            std::snprintf(pong_topic, sizeof(pong_topic), "%s%s", TOPIC_PONG_PREFIX, ctx->edge_id);
            char relay_topic[128];
            std::snprintf(relay_topic, sizeof(relay_topic), "campus/relay/%s", ctx->edge_id);
            mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
            mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
            mosquitto_subscribe(mosq, nullptr, "campus/alert/core_switch", 1);
            mosquitto_subscribe(mosq, nullptr, ping_topic, 0);
            mosquitto_subscribe(mosq, nullptr, pong_topic, 0);
            mosquitto_subscribe(mosq, nullptr, relay_topic, 1);
        }
        publish_edge_status(mosq, ctx, "core");
        flush_store_queue(ctx);
        break;

    case UpstreamKind::BACKUP:
        std::printf("[edge] connected to backup core\n");
        publish_edge_status(up.mosq, ctx, "backup core");
        flush_store_queue(ctx);
        break;

    case UpstreamKind::PEER_EDGE:
        std::printf("[edge] connected to peer edge (%s)\n", up.ip);
        // PEER_ONLY 모드에서 CT 수신을 위해 topology 구독
        mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
        {
            // peer 브로커를 통해 ping 수신 가능하도록 자신의 ping 토픽 구독
            char ping_topic_p[128];
            std::snprintf(ping_topic_p, sizeof(ping_topic_p),
                "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
            mosquitto_subscribe(mosq, nullptr, ping_topic_p, 0);
        }
        publish_edge_status(up.mosq, ctx, "peer edge");
        flush_store_queue(ctx);
        break;
    }
}

static void on_disconnect_upstream(struct mosquitto* /*mosq*/, void* userdata, int rc)
{
    auto* cb = static_cast<UpstreamCallbackCtx*>(userdata);
    auto* ctx = cb->ctx;
    UpstreamConn& up = ctx->upstream_conns[cb->slot];
    up.connected = false;

    const char* kind_str = (up.kind == UpstreamKind::CORE) ? "core" :
                           (up.kind == UpstreamKind::BACKUP) ? "backup core" : "peer edge";
    std::printf("[edge] disconnected from %s (rc=%d)%s\n", kind_str, rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

// CT 메시지 처리 공통 로직 (CORE / PEER_EDGE 모두 사용)
static void handle_topology_message(struct mosquitto* /*mosq*/, void* userdata,
    const struct mosquitto_message* msg)
{
    auto* cb = static_cast<UpstreamCallbackCtx*>(userdata);
    auto* ctx = cb->ctx;

    ConnectionTable ct;
    std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
    if (!connection_table_from_json(json, ct))
        return;

    if (ct.version <= ctx->last_ct_version)
    {
        std::printf("[edge] skip stale CT (remote=%d <= local=%d)\n",
            ct.version, ctx->last_ct_version);
        return;
    }

    // active_core_id 변경 감지 → CORE 슬롯이 있는 경우 재연결
    if (ct.active_core_id[0] != '\0')
    {
        std::string prev_active = ctx->ct_manager->snapshot().active_core_id;
        ctx->ct_manager->setActiveCoreId(ct.active_core_id);

        if (prev_active != ct.active_core_id)
        {
            const NodeEntry* new_core_entry = nullptr;
            for (int j = 0; j < ct.node_count; j++)
            {
                if (std::strncmp(ct.nodes[j].id, ct.active_core_id, UUID_LEN) == 0)
                {
                    new_core_entry = &ct.nodes[j];
                    break;
                }
            }
            UpstreamConn* core_slot = find_upstream_any(ctx, UpstreamKind::CORE);
            if (core_slot && new_core_entry && new_core_entry->ip[0] != '\0'
                && (std::strcmp(new_core_entry->ip, ctx->active_core_ip) != 0
                    || new_core_entry->port != ctx->active_core_port))
            {
                std::printf("[edge] active_core_id changed → reconnect to %s:%d\n",
                    new_core_entry->ip, new_core_entry->port);
                std::strncpy(ctx->active_core_ip, new_core_entry->ip, IP_LEN - 1);
                ctx->active_core_port = new_core_entry->port;
                mosquitto_disconnect(core_slot->mosq);
                mosquitto_connect_async(core_slot->mosq,
                    ctx->active_core_ip, ctx->active_core_port, 60);
                for (int i = 0; i < ctx->upstream_count; i++)
                    ctx->upstream_conns[i].preferred = false;
            }
        }
    }

    ctx->ct_manager->replace(ct);
    ctx->last_ct_version = ct.version;
    std::printf("[edge] CT applied (version=%d, nodes=%d)\n", ct.version, ct.node_count);

    // publisher_client 가 CT를 받을 수 있도록 로컬 재발행
    if (ctx->mosq_local) {
        std::string ct_raw(static_cast<char*>(msg->payload), msg->payloadlen);
        mosquitto_publish(ctx->mosq_local, nullptr, TOPIC_TOPOLOGY,
            (int)ct_raw.size(), ct_raw.c_str(), 1, true);
    }

    send_pings_to_nodes(ctx);
}

static void on_message_upstream(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg)
{
    auto* cb = static_cast<UpstreamCallbackCtx*>(userdata);
    auto* ctx = cb->ctx;
    UpstreamConn& up = ctx->upstream_conns[cb->slot];

    // CT 수신은 CORE / PEER_EDGE 모두 처리 (PEER_ONLY 모드 지원)
    if (std::strcmp(msg->topic, TOPIC_TOPOLOGY) == 0)
    {
        handle_topology_message(mosq, userdata, msg);
        return;
    }

    // Ping/Pong: CORE · PEER_EDGE 모두 처리 (peer-only edge 경유 RTT 지원)
    if (std::strncmp(msg->topic, TOPIC_PING_PREFIX, std::strlen(TOPIC_PING_PREFIX)) == 0)
    {
        process_ping(mosq, ctx, msg);
        return;
    }
    if (std::strncmp(msg->topic, TOPIC_PONG_PREFIX, std::strlen(TOPIC_PONG_PREFIX)) == 0)
    {
        process_pong(ctx, msg);
        return;
    }

    // 이하 제어 메시지는 CORE 슬롯에서만 처리
    if (up.kind != UpstreamKind::CORE)
        return;

    // campus/alert/core_switch 수신: 새 Active Core로 명시적 재연결 (FR-05, FR-10)
    if (std::strcmp(msg->topic, "campus/alert/core_switch") == 0)
    {
        MqttMessage sw = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, sw))
            return;

        char new_ip[IP_LEN] = {};
        int  new_port = 0;
        if (!parse_ip_port(sw.payload.description, new_ip, sizeof(new_ip), &new_port))
        {
            std::fprintf(stderr, "[edge] core_switch: invalid payload '%s'\n",
                sw.payload.description);
            return;
        }

        std::printf("[edge] core_switch: new active core at %s:%d\n", new_ip, new_port);

        if (std::strcmp(new_ip, ctx->active_core_ip) == 0
            && new_port == ctx->active_core_port)
            return;

        std::printf("[edge] core_switch: reconnecting to %s:%d\n", new_ip, new_port);
        std::strncpy(ctx->active_core_ip, new_ip, IP_LEN - 1);
        ctx->active_core_port = new_port;

        UpstreamConn* core_slot = find_upstream_any(ctx, UpstreamKind::CORE);
        if (core_slot && core_slot->mosq)
        {
            mosquitto_disconnect(core_slot->mosq);
            mosquitto_connect_async(core_slot->mosq, ctx->active_core_ip, ctx->active_core_port, 60);
        }
        for (int i = 0; i < ctx->upstream_count; i++)
            ctx->upstream_conns[i].preferred = false;
        return;
    }

    // Core LWT 수신 (W-01): 비 CORE 슬롯을 우선 경로로 승격
    if (std::strncmp(msg->topic, "campus/will/core/", 17) == 0)
    {
        const char* failed_core_id = msg->topic + 17;
        if (!should_failover_on_core_will(*ctx->ct_manager, failed_core_id))
        {
            if (is_backup_core_will(*ctx->ct_manager, failed_core_id))
                std::printf("[edge] backup core down: %s\n", failed_core_id);
            else
                std::printf("[edge] ignoring non-active core down notice: %s\n", failed_core_id);
            return;
        }

        std::printf("[edge] core down: %s\n", failed_core_id);

        up.connected = false;

        // BACKUP 또는 첫 번째 PEER_EDGE 슬롯을 preferred 로 승격
        for (int i = 0; i < ctx->upstream_count; i++)
        {
            if (ctx->upstream_conns[i].kind != UpstreamKind::CORE
                && ctx->upstream_conns[i].mosq
                && ctx->upstream_conns[i].connected)
            {
                ctx->upstream_conns[i].preferred = true;
                std::printf("[edge] switched to %s-preferred mode\n",
                    ctx->upstream_conns[i].kind == UpstreamKind::BACKUP ? "backup" : "peer");
                break;
            }
        }

        // 대체 경로로 저장 큐 즉시 재전송 시도
        if (find_upstream(ctx, UpstreamKind::BACKUP) || find_upstream(ctx, UpstreamKind::PEER_EDGE))
            flush_store_queue(ctx);

        return;
    }

    // Relay 수신 → Core로 전달 (FR-08)
    {
        char relay_prefix[128];
        std::snprintf(relay_prefix, sizeof(relay_prefix), "campus/relay/%s", ctx->edge_id);
        if (std::strcmp(msg->topic, relay_prefix) == 0)
        {
            char fwd_topic[128];
            MqttMessage relayed = {};
            std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
            if (mqtt_message_from_json(json, relayed))
            {
                std::snprintf(fwd_topic, sizeof(fwd_topic), "campus/data/%s", relayed.source.id);
                std::string fwd_json = mqtt_message_to_json(relayed);
                mosquitto_publish(mosq, nullptr, fwd_topic,
                    (int)fwd_json.size(), fwd_json.c_str(), 1, false);
                std::printf("[edge] relay forwarded: %s → %s\n", msg->topic, fwd_topic);
            }
            return;
        }
    }

}

// maybe_establish_peer_conn: pong RTT 측정 후 동적 peer 연결 추가
static void maybe_establish_peer_conn(EdgeContext* ctx, const std::string& peer_node_id)
{
    std::lock_guard<std::mutex> lock(ctx->upstream_mutex);

    // 이미 해당 peer_node_id 슬롯이 있으면 skip
    for (int i = 0; i < ctx->upstream_count; i++)
    {
        UpstreamConn& u = ctx->upstream_conns[i];
        if (u.kind == UpstreamKind::PEER_EDGE
            && std::strncmp(u.peer_node_id, peer_node_id.c_str(), UUID_LEN) == 0)
            return;
    }

    if (ctx->upstream_count >= MAX_UPSTREAM)
    {
        std::fprintf(stderr, "[edge] MAX_UPSTREAM reached, cannot add peer %s\n",
            peer_node_id.c_str());
        return;
    }

    // CT에서 ip:port 조회
    auto entry = ctx->ct_manager->findNode(peer_node_id.c_str());
    if (!entry.has_value() || entry->ip[0] == '\0' || entry->port == 0)
    {
        std::fprintf(stderr, "[edge] peer node %s has no ip:port in CT\n",
            peer_node_id.c_str());
        return;
    }

    int slot_idx = ctx->upstream_count++;
    UpstreamConn& slot = ctx->upstream_conns[slot_idx];
    slot.kind = UpstreamKind::PEER_EDGE;
    slot.port = entry->port;
    slot.slot = slot_idx;
    std::strncpy(slot.ip, entry->ip, IP_LEN - 1);
    std::strncpy(slot.peer_node_id, peer_node_id.c_str(), UUID_LEN - 1);

    auto* cb_ctx = new UpstreamCallbackCtx{ctx, slot_idx};
    ctx->upstream_cb[slot_idx] = cb_ctx;

    char peer_client_id[80];
    std::snprintf(peer_client_id, sizeof(peer_client_id), "%s-peer-%d",
        ctx->edge_id, slot_idx);

    struct mosquitto* mosq_peer = mosquitto_new(peer_client_id, true, cb_ctx);
    if (!mosq_peer)
    {
        ctx->upstream_count--;
        delete cb_ctx;
        ctx->upstream_cb[slot_idx] = nullptr;
        return;
    }

    slot.mosq = mosq_peer;

    mosquitto_connect_callback_set(mosq_peer, on_connect_upstream);
    mosquitto_message_callback_set(mosq_peer, on_message_upstream);
    mosquitto_disconnect_callback_set(mosq_peer, on_disconnect_upstream);

    mosquitto_reconnect_delay_set(mosq_peer, 2, 30, false);
    mosquitto_connect_async(mosq_peer, slot.ip, slot.port, 60);
    mosquitto_loop_start(mosq_peer);

    std::printf("[edge] initiating peer connection to %s at %s:%u\n",
        peer_node_id.c_str(), slot.ip, slot.port);
}

static void on_connect_local(struct mosquitto* mosq, void* userdata, int rc)
{
    if (rc != 0)
    {
        std::fprintf(stderr, "[edge] local broker connect failed (rc=%d)\n", rc);
        return;
    }

    auto* ctx = static_cast<EdgeContext*>(userdata);

    std::printf("[edge] connected to local broker\n");

    char data_topic[128];
    std::snprintf(data_topic, sizeof(data_topic), "%s#", EDGE_DATA_TOPIC_PREFIX);

    int sub_rc = mosquitto_subscribe(mosq, nullptr, data_topic, 0);
    if (sub_rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] local subscribe (data) failed: %s\n",
            mosquitto_strerror(sub_rc));
        return;
    }

    // 인접 Edge 의 status 등록 메시지 전달을 위해 구독
    sub_rc = mosquitto_subscribe(mosq, nullptr, "campus/monitor/status/#", 1);
    if (sub_rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] local subscribe (status) failed: %s\n",
            mosquitto_strerror(sub_rc));
    }

    // peer-only edge 가 local broker 로 응답한 pong 수신을 위해 구독
    {
        char pong_topic_l[128];
        std::snprintf(pong_topic_l, sizeof(pong_topic_l),
            "%s%s", TOPIC_PONG_PREFIX, ctx->edge_id);
        mosquitto_subscribe(mosq, nullptr, pong_topic_l, 0);
    }

    std::printf("[edge] subscribed local topics: %s, campus/monitor/status/#\n", data_topic);
}

static void on_disconnect_local(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc)
{
    std::printf("[edge] disconnected from local broker (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_local(struct mosquitto* /*mosq*/, void* userdata,
    const struct mosquitto_message* msg)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);

    // pong 수신 (peer-only edge 가 local broker 로 응답한 경우)
    if (std::strncmp(msg->topic, TOPIC_PONG_PREFIX, std::strlen(TOPIC_PONG_PREFIX)) == 0)
    {
        process_pong(ctx, msg);
        return;
    }

    // campus/monitor/status/# → 인접 Edge 등록 메시지를 upstream(Core) 으로 전달
    if (std::strncmp(msg->topic, TOPIC_STATUS_PREFIX, std::strlen(TOPIC_STATUS_PREFIX)) == 0)
    {
        // 자신의 status 는 forwarding 하지 않음 (on_connect_upstream 에서 직접 발행)
        const char* node_id_in_topic = msg->topic + std::strlen(TOPIC_STATUS_PREFIX);
        if (std::strncmp(node_id_in_topic, ctx->edge_id, UUID_LEN - 1) == 0)
            return;

        UpstreamConn* up = find_upstream(ctx, UpstreamKind::CORE);
        if (!up) up = find_upstream(ctx, UpstreamKind::BACKUP);
        if (up && up->mosq)
        {
            // campus/monitor/status_relay/<this_edge_id>/<peer_node_id>
            // Core 가 이 토픽을 받으면 peer→node 링크를 생성 (Core→node 직결 링크 아님)
            const char* peer_node_id = msg->topic + std::strlen(TOPIC_STATUS_PREFIX);
            char relay_topic[256];
            std::snprintf(relay_topic, sizeof(relay_topic),
                "%s%s/%s", TOPIC_STATUS_RELAY_PREFIX, ctx->edge_id, peer_node_id);
            mosquitto_publish(up->mosq, nullptr, relay_topic,
                msg->payloadlen, msg->payload, 1, false);
            std::printf("[edge] forwarded peer status: %s\n", relay_topic);
        }
        return;
    }

    // campus/data/# → 로컬 CCTV 이벤트 upstream 전달
    if (std::strncmp(msg->topic, EDGE_DATA_TOPIC_PREFIX, std::strlen(EDGE_DATA_TOPIC_PREFIX)) != 0)
        return;

    std::string payload;
    if (msg->payload != nullptr && msg->payloadlen > 0)
        payload.assign(static_cast<char*>(msg->payload), msg->payloadlen);

    std::printf("[edge][local] event received\n");
    std::printf("  topic   : %s\n", msg->topic);
    std::printf("  payload : %s\n", payload.c_str());

    handle_local_event_delivery(ctx, msg->topic, payload);
}

// main =====================================================

int main(int argc, char* argv[])
{
    // 인수: <broker_host> <broker_port> <core_ip> <core_port> [backup_core_ip] [backup_core_port]
    if (argc < 5)
    {
        std::fprintf(stderr,
            "usage: %s <broker_host> <broker_port> <core_ip> <core_port>"
            " [backup_core_ip] [backup_core_port]\n",
            argv[0]);
        return 1;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* broker_host    = argv[1];
    int broker_port            = std::atoi(argv[2]);
    const char* core_ip        = argv[3];
    int core_port              = std::atoi(argv[4]);
    const char* backup_core_ip = (argc > 5) ? argv[5] : "";
    int backup_port            = (argc > 6) ? std::atoi(argv[6]) : 1883;

    // PEER_ONLY: Core 에 직접 연결하지 않고 PEER_EDGE 만 사용
    bool peer_only = false;
    if (const char* po = std::getenv("EDGE_PEER_ONLY"))
        peer_only = (po[0] == '1');

    // 정적 peer 목록 (EDGE_UPSTREAM_PEERS=ip:port,ip:port)
    std::string peers_env;
    if (const char* ep = std::getenv("EDGE_UPSTREAM_PEERS"))
        peers_env = ep;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. 기본 컨텍스트 초기화
    EdgeContext ctx{};
    ctx.node_port = (uint16_t)broker_port;
    if (const char* ep = std::getenv("EDGE_NODE_PORT")) {
        int p = std::atoi(ep);
        if (p > 0 && p <= 65535) ctx.node_port = (uint16_t)p;
    }
    ctx.mosq_local = nullptr;
    std::strncpy(ctx.active_core_ip, core_ip, IP_LEN - 1);
    ctx.active_core_port = core_port;

    // 2. core 방향 outbound IP 자동 감지
    if (!get_outbound_ip(core_ip, core_port, ctx.node_ip, sizeof(ctx.node_ip)))
    {
        std::fprintf(stderr, "[edge] failed to detect outbound IP toward %s\n", core_ip);
        return 1;
    }

    // 3. edge_id 고정
    {
        const char* suffix = std::getenv("EDGE_ID_SUFFIX");
        char edge_seed[64];
        std::snprintf(edge_seed, sizeof(edge_seed), "edge:%s:%u%s",
            ctx.node_ip, ctx.node_port, suffix ? suffix : "");
        uuid_generate_deterministic(edge_seed, ctx.edge_id);
    }

    // 4. 로컬 CT 초기화
    ConnectionTableManager ct_manager;
    ctx.ct_manager = &ct_manager;
    ct_manager.init("", "");

    // 5. mosquitto 초기화
    mosquitto_lib_init();

    // 6. upstream 슬롯 구성 ─────────────────────────────────────────────────

    // 6-a. CORE 슬롯 (peer_only 모드가 아닐 때)
    if (!peer_only)
    {
        int idx = ctx.upstream_count++;
        UpstreamConn& s = ctx.upstream_conns[idx];
        s.kind = UpstreamKind::CORE;
        s.slot = idx;
        std::strncpy(s.ip, core_ip, IP_LEN - 1);
        s.port = (uint16_t)core_port;

        auto* cb = new UpstreamCallbackCtx{&ctx, idx};
        ctx.upstream_cb[idx] = cb;

        struct mosquitto* m = mosquitto_new(ctx.edge_id, true, cb);
        if (!m) {
            std::fprintf(stderr, "[edge] mosquitto_new (core) failed\n");
            delete cb;
            mosquitto_lib_cleanup();
            return 1;
        }
        s.mosq = m;

        // LWT 설정 (W-02): CORE 슬롯에만
        char lwt_topic[128];
        std::snprintf(lwt_topic, sizeof(lwt_topic), "%s%s",
            TOPIC_LWT_NODE_PREFIX, ctx.edge_id);
        std::string lwt_json = build_edge_lwt_json(&ctx);
        mosquitto_will_set(m, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // 6-b. BACKUP 슬롯
    if (!peer_only && backup_core_ip[0] != '\0')
    {
        int idx = ctx.upstream_count++;
        UpstreamConn& s = ctx.upstream_conns[idx];
        s.kind = UpstreamKind::BACKUP;
        s.slot = idx;
        std::strncpy(s.ip, backup_core_ip, IP_LEN - 1);
        s.port = (uint16_t)backup_port;

        char backup_id[64];
        std::snprintf(backup_id, sizeof(backup_id), "%s-backup", ctx.edge_id);

        auto* cb = new UpstreamCallbackCtx{&ctx, idx};
        ctx.upstream_cb[idx] = cb;

        struct mosquitto* m = mosquitto_new(backup_id, true, cb);
        if (!m) {
            std::fprintf(stderr, "[edge] mosquitto_new (backup) failed\n");
            ctx.upstream_count--;
            delete cb;
            ctx.upstream_cb[idx] = nullptr;
            // backup 실패는 경고만 (필수 아님)
        } else {
            s.mosq = m;
        }
    }

    // 6-c. 정적 PEER_EDGE 슬롯 (EDGE_UPSTREAM_PEERS 또는 EDGE_PEER_ONLY 모드)
    if (!peers_env.empty())
    {
        std::string token;
        std::string tmp = peers_env;
        while (!tmp.empty() && ctx.upstream_count < MAX_UPSTREAM)
        {
            size_t comma = tmp.find(',');
            token = (comma == std::string::npos) ? tmp : tmp.substr(0, comma);
            tmp   = (comma == std::string::npos) ? "" : tmp.substr(comma + 1);

            char peer_ip[IP_LEN] = {};
            int  peer_port = 0;
            if (!parse_ip_port(token.c_str(), peer_ip, sizeof(peer_ip), &peer_port))
                continue;

            int idx = ctx.upstream_count++;
            UpstreamConn& s = ctx.upstream_conns[idx];
            s.kind = UpstreamKind::PEER_EDGE;
            s.slot = idx;
            std::strncpy(s.ip, peer_ip, IP_LEN - 1);
            s.port = (uint16_t)peer_port;

            char peer_id[80];
            std::snprintf(peer_id, sizeof(peer_id), "%s-peer-%d", ctx.edge_id, idx);

            auto* cb = new UpstreamCallbackCtx{&ctx, idx};
            ctx.upstream_cb[idx] = cb;

            struct mosquitto* m = mosquitto_new(peer_id, true, cb);
            if (!m) {
                ctx.upstream_count--;
                delete cb;
                ctx.upstream_cb[idx] = nullptr;
                continue;
            }
            s.mosq = m;
        }
    }

    if (ctx.upstream_count == 0)
    {
        std::fprintf(stderr, "[edge] no upstream connections configured\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    // 7. 로컬 브로커 클라이언트 생성
    char local_client_id[64];
    std::snprintf(local_client_id, sizeof(local_client_id), "%s-local", ctx.edge_id);

    struct mosquitto* mosq_local = mosquitto_new(local_client_id, true, &ctx);
    if (!mosq_local)
    {
        std::fprintf(stderr, "[edge] mosquitto_new (local) failed\n");
        for (int i = 0; i < ctx.upstream_count; i++) {
            if (ctx.upstream_conns[i].mosq)
                mosquitto_destroy(ctx.upstream_conns[i].mosq);
            delete ctx.upstream_cb[i];
        }
        mosquitto_lib_cleanup();
        return 1;
    }
    ctx.mosq_local = mosq_local;

    // 8. 콜백 등록
    for (int i = 0; i < ctx.upstream_count; i++)
    {
        if (!ctx.upstream_conns[i].mosq) continue;
        struct mosquitto* m = ctx.upstream_conns[i].mosq;
        mosquitto_connect_callback_set(m, on_connect_upstream);
        mosquitto_message_callback_set(m, on_message_upstream);
        mosquitto_disconnect_callback_set(m, on_disconnect_upstream);
        mosquitto_reconnect_delay_set(m, 2, 30, false);
    }

    mosquitto_connect_callback_set(mosq_local, on_connect_local);
    mosquitto_message_callback_set(mosq_local, on_message_local);
    mosquitto_disconnect_callback_set(mosq_local, on_disconnect_local);

    // 9. 연결

    // 슬롯[0] (첫 번째 upstream) 은 blocking connect
    {
        UpstreamConn& s0 = ctx.upstream_conns[0];
        if (!s0.mosq) {
            std::fprintf(stderr, "[edge] upstream[0] has no mosq client\n");
            for (int i = 0; i < ctx.upstream_count; i++) {
                if (ctx.upstream_conns[i].mosq)
                    mosquitto_destroy(ctx.upstream_conns[i].mosq);
                delete ctx.upstream_cb[i];
            }
            mosquitto_destroy(mosq_local);
            mosquitto_lib_cleanup();
            return 1;
        }
        int rc = mosquitto_connect(s0.mosq, s0.ip, s0.port, 60);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] connect to upstream[0] (%s:%d) failed: %s\n",
                s0.ip, s0.port, mosquitto_strerror(rc));
            for (int i = 0; i < ctx.upstream_count; i++) {
                if (ctx.upstream_conns[i].mosq)
                    mosquitto_destroy(ctx.upstream_conns[i].mosq);
                delete ctx.upstream_cb[i];
            }
            mosquitto_destroy(mosq_local);
            mosquitto_lib_cleanup();
            return 1;
        }
    }

    // 나머지 upstream 은 async connect
    for (int i = 1; i < ctx.upstream_count; i++)
    {
        if (!ctx.upstream_conns[i].mosq) continue;
        UpstreamConn& s = ctx.upstream_conns[i];
        int rc = mosquitto_connect_async(s.mosq, s.ip, s.port, 60);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] connect_async to upstream[%d] (%s:%d) failed: %s\n",
                i, s.ip, s.port, mosquitto_strerror(rc));
        }
    }

    // 로컬 브로커 연결
    {
        mosquitto_reconnect_delay_set(mosq_local, 2, 30, false);
        int rc = mosquitto_connect(mosq_local, broker_host, broker_port, 60);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] connect to local broker failed: %s\n",
                mosquitto_strerror(rc));
            for (int i = 0; i < ctx.upstream_count; i++) {
                if (ctx.upstream_conns[i].mosq)
                    mosquitto_destroy(ctx.upstream_conns[i].mosq);
                delete ctx.upstream_cb[i];
            }
            mosquitto_destroy(mosq_local);
            mosquitto_lib_cleanup();
            return 1;
        }
    }

    // 10. 이벤트 루프 시작
    for (int i = 0; i < ctx.upstream_count; i++) {
        if (ctx.upstream_conns[i].mosq)
            mosquitto_loop_start(ctx.upstream_conns[i].mosq);
    }
    mosquitto_loop_start(mosq_local);

    // 기동 정보 출력
    {
        UpstreamConn* core_slot = find_upstream_any(&ctx, UpstreamKind::CORE);
        UpstreamConn* bk_slot   = find_upstream_any(&ctx, UpstreamKind::BACKUP);
        int peer_count = 0;
        for (int i = 0; i < ctx.upstream_count; i++)
            if (ctx.upstream_conns[i].kind == UpstreamKind::PEER_EDGE)
                peer_count++;
        std::printf("[edge] %s  local=%s:%d  core=%s  backup=%s  peers=%d\n",
            ctx.edge_id, ctx.node_ip, broker_port,
            core_slot ? core_slot->ip : "(none)",
            bk_slot   ? bk_slot->ip  : "(none)",
            peer_count);
    }

    while (g_running)
    {
        flush_store_queue(&ctx);
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, nullptr);
    }

    std::printf("[edge] shutting down\n");

    // 첫 번째 유효 upstream 을 통해 down notice 발행
    for (int i = 0; i < ctx.upstream_count; i++) {
        if (ctx.upstream_conns[i].mosq && ctx.upstream_conns[i].connected) {
            publish_edge_down_notice(ctx.upstream_conns[i].mosq, &ctx);
            break;
        }
    }
    {
        struct timespec flush_ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&flush_ts, nullptr);
    }

    // 종료: 동적 추가된 슬롯 포함 모든 upstream 정리
    for (int i = ctx.upstream_count - 1; i >= 0; i--)
    {
        if (ctx.upstream_conns[i].mosq)
        {
            mosquitto_loop_stop(ctx.upstream_conns[i].mosq, true);
            mosquitto_destroy(ctx.upstream_conns[i].mosq);
            ctx.upstream_conns[i].mosq = nullptr;
        }
        delete ctx.upstream_cb[i];
        ctx.upstream_cb[i] = nullptr;
    }

    mosquitto_loop_stop(mosq_local, true);
    mosquitto_destroy(mosq_local);

    mosquitto_lib_cleanup();
    return 0;
}
