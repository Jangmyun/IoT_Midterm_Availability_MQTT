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

// Global State =====================================================
static volatile bool g_running = true;

struct QueuedEvent
{
    char topic[128];
    MqttMessage msg;
};

struct EdgeContext
{
    char edge_id[UUID_LEN];
    char node_ip[IP_LEN];
    uint16_t node_port;
    ConnectionTableManager* ct_manager;

    // upstream publish를 위해 보관
    struct mosquitto* mosq_core;
    struct mosquitto* mosq_backup;

    bool core_connected;
    bool backup_connected;
    bool prefer_backup;

    // store-and-forward queue
    std::deque<QueuedEvent> store_queue;
    std::mutex queue_mutex;
    std::mutex flush_mutex;

    int last_ct_version = 0;  // 마지막 수신한 CT 버전 (구버전 무시용)

    // 현재 연결 중인 Active Core 주소 (core_switch 재연결 판단용)
    char active_core_ip[IP_LEN];
    int  active_core_port;

    // RTT 측정: target_node_id → ping 발송 시각 (FR-08)
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> ping_send_times;
    std::mutex ping_mutex;

    // 현재 선택된 최적 Relay Node UUID (없으면 빈 문자열) (FR-08)
    char relay_node_id[UUID_LEN];
};

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


// core / backup 공통 등록 함수
static void publish_edge_status(struct mosquitto* mosq, EdgeContext* ctx, const char* label)
{
    // Core에 자신 등록: description 필드에 "ip:port" 인코딩
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
    // Core LWT를 받아 backup 우선 모드가 된 경우
    if (ctx->prefer_backup)
    {
        if (ctx->backup_connected && ctx->mosq_backup)
        {
            if (publish_to_upstream(ctx->mosq_backup, "backup core", topic, msg))
                return true;
        }

        if (ctx->core_connected && ctx->mosq_core)
        {
            if (publish_to_upstream(ctx->mosq_core, "core", topic, msg))
                return true;
        }

        return false;
    }

    // 평상시에는 Core 우선 전송
    if (ctx->core_connected && ctx->mosq_core)
    {
        if (publish_to_upstream(ctx->mosq_core, "core", topic, msg))
            return true;
    }

    // Core가 안 되면 Backup Core로 전송
    if (ctx->backup_connected && ctx->mosq_backup)
    {
        if (publish_to_upstream(ctx->mosq_backup, "backup core", topic, msg))
            return true;
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

    // 먼저 오래된 큐가 있으면 순서를 보존하기 위해 flush 시도
    if (has_pending_queue(ctx))
    {
        flush_store_queue(ctx);

        // 아직도 큐가 남아있다면 새 이벤트도 뒤에 저장
        if (has_pending_queue(ctx))
        {
            queue_event(ctx, topic, event_msg);
            return;
        }
    }

    // 현재 이벤트 즉시 전송 시도
    if (forward_message_upstream(ctx, topic, event_msg))
    {
        return;
    }

    // 전송 실패 시 store-and-forward 큐에 저장
    queue_event(ctx, topic, event_msg);
}

// CT 수신 후 ONLINE NODE에 Ping 발송 → RTT 측정 시작 (FR-08)
static void send_pings_to_nodes(EdgeContext* ctx)
{
    if (!ctx->mosq_core || !ctx->core_connected)
        return;

    ConnectionTable ct = ctx->ct_manager->snapshot();
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& n = ct.nodes[i];
        if (n.role != NODE_ROLE_NODE)              continue;
        if (n.status != NODE_STATUS_ONLINE)        continue;
        if (std::strncmp(n.id, ctx->edge_id, UUID_LEN) == 0) continue;  // 자신 제외

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
        mosquitto_publish(ctx->mosq_core, nullptr, topic,
            (int)json.size(), json.c_str(), 0, false);

        std::printf("[edge] ping sent to %s\n", n.id);
    }
}

// Callbacks =====================================================

static void on_connect_core(struct mosquitto* mosq, void* userdata, int rc)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);

    if (rc != 0)
    {
        ctx->core_connected = false;
        std::fprintf(stderr, "[edge] core connect failed (rc=%d)\n", rc);
        return;
    }

    ctx->core_connected = true;
    std::printf("[edge] connected to core\n");
    ctx->prefer_backup = false;
    std::printf("[edge] core is primary again\n");
    // 구독
    char ping_topic[128];
    std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
    char pong_topic[128];
    std::snprintf(pong_topic, sizeof(pong_topic), "%s%s", TOPIC_PONG_PREFIX, ctx->edge_id);
    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, "campus/alert/core_switch", 1);
    mosquitto_subscribe(mosq, nullptr, ping_topic, 0);
    mosquitto_subscribe(mosq, nullptr, pong_topic, 0);  // Pong 수신: RTT 측정용 (FR-08)

    publish_edge_status(mosq, ctx, "core");

    // core가 살아났으면 저장된 메시지 재전송 시도
    flush_store_queue(ctx);
}

static void on_disconnect_core(struct mosquitto* /*mosq*/, void* userdata, int rc)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);
    ctx->core_connected = false;

    std::printf("[edge] disconnected from core (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_core(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);

    // CT 수신 (M-04): 로컬 CT 갱신 + active_core_id 변경 감지
    if (std::strcmp(msg->topic, TOPIC_TOPOLOGY) == 0)
    {
        ConnectionTable ct;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (connection_table_from_json(json, ct))
        {
            if (ct.version <= ctx->last_ct_version)
            {
                std::printf("[edge] skip stale CT (remote=%d <= local=%d)\n",
                    ct.version, ctx->last_ct_version);
                return;
            }

            // active_core_id 변경 감지 → 새 Active Core로 재연결
            if (ct.active_core_id[0] != '\0')
            {
                std::string prev_active = ctx->ct_manager->snapshot().active_core_id;
                ctx->ct_manager->setActiveCoreId(ct.active_core_id);

                if (prev_active != ct.active_core_id)
                {
                    auto new_core = ctx->ct_manager->findNode(ct.active_core_id);
                    if (new_core && new_core->ip[0] != '\0'
                        && (std::strcmp(new_core->ip, ctx->active_core_ip) != 0
                            || new_core->port != ctx->active_core_port))
                    {
                        std::printf("[edge] active_core_id changed → reconnect to %s:%d\n",
                            new_core->ip, new_core->port);
                        std::strncpy(ctx->active_core_ip, new_core->ip, IP_LEN - 1);
                        ctx->active_core_port = new_core->port;
                        mosquitto_disconnect(ctx->mosq_core);
                        mosquitto_connect_async(ctx->mosq_core,
                            ctx->active_core_ip, ctx->active_core_port, 60);
                        ctx->prefer_backup = false;
                    }
                }
            }

            for (int i = 0; i < ct.node_count; i++)
            {
                if (!ctx->ct_manager->addNode(ct.nodes[i]))
                    ctx->ct_manager->updateNode(ct.nodes[i]);
            }
            for (int i = 0; i < ct.link_count; i++)
                ctx->ct_manager->addLink(ct.links[i]);
            ctx->last_ct_version = ct.version;
            std::printf("[edge] CT applied (version=%d, nodes=%d)\n", ct.version, ct.node_count);
            send_pings_to_nodes(ctx);  // RTT 측정 시작 (FR-08)
        }
        return;
    }

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

        // 이미 해당 Core에 연결 중이면 무시
        if (std::strcmp(new_ip, ctx->active_core_ip) == 0
            && new_port == ctx->active_core_port)
            return;

        std::printf("[edge] core_switch: reconnecting to %s:%d\n", new_ip, new_port);
        std::strncpy(ctx->active_core_ip, new_ip, IP_LEN - 1);
        ctx->active_core_port = new_port;
        mosquitto_disconnect(ctx->mosq_core);
        mosquitto_connect_async(ctx->mosq_core, ctx->active_core_ip, ctx->active_core_port, 60);
        ctx->prefer_backup = false;
        return;
    }

    // Core LWT 수신 (W-01): backup core를 사실상 1순위로 승격
    if (std::strncmp(msg->topic, "campus/will/core/", 17) == 0)
    {
        std::printf("[edge] core down: %s\n", msg->topic + 17);

        // active core 경로는 더 이상 우선 사용하지 않음
        ctx->core_connected = false;
        ctx->prefer_backup = true;

        std::printf("[edge] switched to backup-preferred mode\n");

        // backup이 이미 연결되어 있다면 저장된 메시지부터 바로 재전송 시도
        if (ctx->backup_connected && ctx->mosq_backup)
        {
            flush_store_queue(ctx);
        }

        return;
    }

    // Pong 수신 → RTT 계산 + LinkEntry 갱신 + Relay Node 선택 (FR-08, M-02)
    if (std::strncmp(msg->topic, TOPIC_PONG_PREFIX, std::strlen(TOPIC_PONG_PREFIX)) == 0)
    {
        MqttMessage pong;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, pong))
            return;

        // 자신에게 온 pong인지 확인
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
        ctx->ct_manager->addLink(link);  // 이미 존재하면 RTT 갱신

        std::string best = select_relay_node(*ctx->ct_manager, ctx->edge_id);
        if (!best.empty())
        {
            std::strncpy(ctx->relay_node_id, best.c_str(), UUID_LEN - 1);
            ctx->relay_node_id[UUID_LEN - 1] = '\0';
            std::printf("[edge] relay node selected: %s\n", ctx->relay_node_id);
        }
        return;
    }

    // Ping 수신 → Pong 응답 (M-01 / M-02)
    if (std::strncmp(msg->topic, TOPIC_PING_PREFIX, std::strlen(TOPIC_PING_PREFIX)) == 0)
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
        mosquitto_publish(mosq, nullptr, pong_topic,
            (int)pong_json.size(), pong_json.c_str(), 0, false);
        return;
    }
}

// 왜 필요한지를 살펴보기
static void on_connect_local(struct mosquitto* mosq, void* userdata, int rc)
{
    if (rc != 0)
    {
        std::fprintf(stderr, "[edge] local broker connect failed (rc=%d)\n", rc);
        return;
    }

    auto* ctx = static_cast<EdgeContext*>(userdata);
    (void)ctx;

    std::printf("[edge] connected to local broker\n");

    char topic[128];
    std::snprintf(topic, sizeof(topic), "%s#", EDGE_DATA_TOPIC_PREFIX);

    int sub_rc = mosquitto_subscribe(mosq, nullptr, topic, 0);
    if (sub_rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] local subscribe failed: %s\n",
            mosquitto_strerror(sub_rc));
        return;
    }

    std::printf("[edge] subscribed local topic: %s\n", topic);
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

    if (std::strncmp(msg->topic, EDGE_DATA_TOPIC_PREFIX, std::strlen(EDGE_DATA_TOPIC_PREFIX)) != 0)
    {
        return;
    }

    std::string payload;
    if (msg->payload != nullptr && msg->payloadlen > 0)
    {
        payload.assign(static_cast<char*>(msg->payload), msg->payloadlen);
    }

    std::printf("[edge][local] event received\n");
    std::printf("  topic   : %s\n", msg->topic);
    std::printf("  payload : %s\n", payload.c_str());

    handle_local_event_delivery(ctx, msg->topic, payload);
}

// backup core 연결 콜백
static void on_connect_backup(struct mosquitto* mosq, void* userdata, int rc)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);

    if (rc != 0)
    {
        ctx->backup_connected = false;
        std::fprintf(stderr, "[edge] backup core connect failed (rc=%d)\n", rc);
        return;
    }

    ctx->backup_connected = true;
    std::printf("[edge] connected to backup core\n");

    // 구독
    char ping_topic[128];
    std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, ping_topic, 0);

    publish_edge_status(mosq, ctx, "backup core");

    // backup이 살아났으면 저장된 메시지 재전송 시도
    flush_store_queue(ctx);
}

static void on_disconnect_backup(struct mosquitto* /*mosq*/, void* userdata, int rc)
{
    auto* ctx = static_cast<EdgeContext*>(userdata);
    ctx->backup_connected = false;

    std::printf("[edge] disconnected from backup core (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_backup(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg)
{
    // 현재 단계에서는 backup core도 core와 동일한 제어 메시지 처리 로직을 사용
    on_message_core(mosq, userdata, msg);
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
    setvbuf(stdout, nullptr, _IOLBF, 0);  // 테스트 스크립트가 로그를 실시간 grep할 수 있도록 line-buffered 설정
    const char* broker_host = argv[1];
    int broker_port = std::atoi(argv[2]);
    const char* core_ip = argv[3];
    int core_port = std::atoi(argv[4]);
    const char* backup_core_ip = (argc > 5) ? argv[5] : "";
    int backup_port = (argc > 6) ? std::atoi(argv[6]) : 1883;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. edge_id UUID 생성
    EdgeContext ctx{};
    uuid_generate(ctx.edge_id);
    ctx.node_port = (uint16_t)broker_port; // 인접 노드들이 접속할 로컬 포트
    ctx.mosq_core = nullptr;
    ctx.mosq_backup = nullptr;
    ctx.core_connected = false;
    ctx.backup_connected = false;
    ctx.prefer_backup = false;
    std::strncpy(ctx.active_core_ip, core_ip, IP_LEN - 1);
    ctx.active_core_port = core_port;

    // 2. core 방향 outbound IP 자동 감지
    if (!get_outbound_ip(core_ip, core_port, ctx.node_ip, sizeof(ctx.node_ip)))
    {
        std::fprintf(stderr, "[edge] failed to detect outbound IP toward %s\n", core_ip);
        return 1;
    }

    // 3. 로컬 CT 초기화 (core로부터 수신 전 빈 상태)
    ConnectionTableManager ct_manager;
    ctx.ct_manager = &ct_manager;
    ct_manager.init("", "");

    // 4. mosquitto 초기화
    mosquitto_lib_init();

    // 5. core 연결용 클라이언트 생성
    struct mosquitto* mosq_core = mosquitto_new(ctx.edge_id, true, &ctx);
    if (!mosq_core)
    {
        std::fprintf(stderr, "[edge] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    ctx.mosq_core = mosq_core;

    // 5-1. local broker 연결용 클라이언트 생성
    char local_client_id[64];
    std::snprintf(local_client_id, sizeof(local_client_id), "%s-local", ctx.edge_id);

    struct mosquitto* mosq_local = mosquitto_new(local_client_id, true, &ctx);
    if (!mosq_local)
    {
        std::fprintf(stderr, "[edge] mosquitto_new for local failed\n");
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 5-2. backup core 연결용 클라이언트 생성
    struct mosquitto* mosq_backup = nullptr;
    if (backup_core_ip[0] != '\0')
    {
        char backup_client_id[64];
        std::snprintf(backup_client_id, sizeof(backup_client_id), "%s-backup", ctx.edge_id);

        mosq_backup = mosquitto_new(backup_client_id, true, &ctx);
        if (!mosq_backup)
        {
            std::fprintf(stderr, "[edge] mosquitto_new for backup failed\n");
            mosquitto_destroy(mosq_local);
            mosquitto_destroy(mosq_core);
            mosquitto_lib_cleanup();
            return 1;
        }
        ctx.mosq_backup = mosq_backup;
    }

    // 6. LWT 설정 (W-02): 비정상 종료 시 core가 OFFLINE 처리
    {
        MqttMessage lwt = {};
        std::strncpy(lwt.msg_id, ctx.edge_id, UUID_LEN - 1);
        lwt.msg_id[UUID_LEN - 1] = '\0';

        lwt.type = MSG_TYPE_LWT_NODE;
        lwt.source.role = NODE_ROLE_NODE;
        std::strncpy(lwt.source.id, ctx.edge_id, UUID_LEN - 1);
        lwt.source.id[UUID_LEN - 1] = '\0';

        lwt.delivery = { 1, false, false };

        char lwt_topic[128];
        std::snprintf(lwt_topic, sizeof(lwt_topic), "%s%s",
            TOPIC_LWT_NODE_PREFIX, ctx.edge_id);

        std::string lwt_json = mqtt_message_to_json(lwt);
        mosquitto_will_set(mosq_core, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // backup core에도 동일한 LWT 설정
    if (mosq_backup)
    {
        MqttMessage lwt_backup = {};
        std::strncpy(lwt_backup.msg_id, ctx.edge_id, UUID_LEN - 1);
        lwt_backup.msg_id[UUID_LEN - 1] = '\0';

        lwt_backup.type = MSG_TYPE_LWT_NODE;
        lwt_backup.source.role = NODE_ROLE_NODE;
        std::strncpy(lwt_backup.source.id, ctx.edge_id, UUID_LEN - 1);
        lwt_backup.source.id[UUID_LEN - 1] = '\0';

        lwt_backup.delivery = { 1, false, false };

        char lwt_topic_backup[128];
        std::snprintf(lwt_topic_backup, sizeof(lwt_topic_backup), "%s%s",
            TOPIC_LWT_NODE_PREFIX, ctx.edge_id);

        std::string lwt_json_backup = mqtt_message_to_json(lwt_backup);
        mosquitto_will_set(mosq_backup, lwt_topic_backup,
            (int)lwt_json_backup.size(), lwt_json_backup.c_str(), 1, false);
    }

    // 7. 콜백 등록
    mosquitto_connect_callback_set(mosq_core, on_connect_core);
    mosquitto_message_callback_set(mosq_core, on_message_core);
    mosquitto_disconnect_callback_set(mosq_core, on_disconnect_core);

    mosquitto_connect_callback_set(mosq_local, on_connect_local);
    mosquitto_message_callback_set(mosq_local, on_message_local);
    mosquitto_disconnect_callback_set(mosq_local, on_disconnect_local);

    if (mosq_backup)
    {
        mosquitto_connect_callback_set(mosq_backup, on_connect_backup);
        mosquitto_message_callback_set(mosq_backup, on_message_backup);
        mosquitto_disconnect_callback_set(mosq_backup, on_disconnect_backup);
    }

    // 8. Core 브로커 연결 (auto-reconnect 활성화)
    mosquitto_reconnect_delay_set(mosq_core, 2, 30, false);
    int rc = mosquitto_connect(mosq_core, core_ip, core_port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] connect to core failed: %s\n",
            mosquitto_strerror(rc));
        if (mosq_backup)
            mosquitto_destroy(mosq_backup);
        mosquitto_destroy(mosq_local);
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 8-1. Local Broker 연결
    mosquitto_reconnect_delay_set(mosq_local, 2, 30, false);
    int rc_local = mosquitto_connect(mosq_local, broker_host, broker_port, 60);
    if (rc_local != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] connect to local broker failed: %s\n",
            mosquitto_strerror(rc_local));
        if (mosq_backup)
            mosquitto_destroy(mosq_backup);
        mosquitto_destroy(mosq_local);
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 8-2. Backup Core 연결
    if (mosq_backup)
    {
        mosquitto_reconnect_delay_set(mosq_backup, 2, 30, false);
        int rc_backup = mosquitto_connect(mosq_backup, backup_core_ip, backup_port, 60);
        if (rc_backup != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] connect to backup core failed: %s\n",
                mosquitto_strerror(rc_backup));
            mosquitto_destroy(mosq_backup);
            mosq_backup = nullptr;
            ctx.mosq_backup = nullptr;
            ctx.backup_connected = false;
        }
    }

    // 9. 이벤트 루프 시작
    mosquitto_loop_start(mosq_core);
    mosquitto_loop_start(mosq_local);
    if (mosq_backup)
    {
        mosquitto_loop_start(mosq_backup);
    }

    std::printf("[edge] %s  local=%s:%d  core=%s:%d  backup=%s\n",
        ctx.edge_id, ctx.node_ip, broker_port,
        core_ip, core_port,
        backup_core_ip[0] ? backup_core_ip : "(none)");

    // mosq_local (broker_host:broker_port) 연결 — 로컬 CCTV 이벤트 수집
    // mosq_backup (backup_core_ip:backup_port) 연결 — Backup Core 유지

    while (g_running)
    {
        // 주기적으로 저장된 큐 flush 시도
        flush_store_queue(&ctx);

        struct timespec ts = { 1, 0 };
        nanosleep(&ts, nullptr);
    }

    std::printf("[edge] shutting down\n");

    if (mosq_backup)
    {
        mosquitto_loop_stop(mosq_backup, true);
        mosquitto_destroy(mosq_backup);
    }

    mosquitto_loop_stop(mosq_local, true);
    mosquitto_destroy(mosq_local);

    mosquitto_loop_stop(mosq_core, true);
    mosquitto_destroy(mosq_core);

    mosquitto_lib_cleanup();
    return 0;
}