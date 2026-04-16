#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"

// Global State =====================================================
static volatile bool g_running = true;

// message.h에는 전체 wildcard만 있으므로 edge 쪽 prefix/topic 비교용으로 따로 둠
static const char* TOPIC_DATA_PREFIX = "campus/data/";
static const char* TOPIC_RELAY_PREFIX = "campus/relay/";
static const char* TOPIC_CORE_SWITCH = "campus/alert/core_switch";

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

    // Core로부터 받은 최신 CT 캐시
    ConnectionTable latest_ct;
    bool has_latest_ct;
    std::mutex ct_mutex;

    // upstream publish를 위해 보관
    struct mosquitto* mosq_core;
    struct mosquitto* mosq_backup;

    bool core_connected;
    bool backup_connected;
    bool prefer_backup;

    char backup_ip[IP_LEN];
    uint16_t backup_port;
    bool backup_endpoint_known;
    bool backup_loop_started;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_ping_sent_at;
    std::mutex ping_mutex;

    char relay_node_id[UUID_LEN];
    bool relay_selected;
    float relay_rtt_ms;
    int relay_hop_to_core;
    std::mutex relay_mutex;

    // store-and-forward queue
    std::deque<QueuedEvent> store_queue;
    std::mutex queue_mutex;
    std::mutex flush_mutex;
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

static std::string to_lower_copy(const std::string& s)
{
    std::string x = s;
    std::transform(x.begin(), x.end(), x.begin(),
        [](unsigned char c)
        { return (char)std::tolower(c); });
    return x;
}

static MsgType infer_msg_type(const char* topic, const std::string& payload)
{
    std::string t = to_lower_copy(topic ? topic : "");
    std::string p = to_lower_copy(payload);

    if (t.find("intrusion") != std::string::npos || p.find("intrusion") != std::string::npos)
        return MSG_TYPE_INTRUSION;

    if (t.find("door") != std::string::npos || p.find("door") != std::string::npos)
        return MSG_TYPE_DOOR_FORCED;

    return MSG_TYPE_MOTION;
}

static MsgPriority infer_priority(MsgType type)
{
    if (type == MSG_TYPE_INTRUSION || type == MSG_TYPE_DOOR_FORCED)
        return PRIORITY_HIGH;
    if (type == MSG_TYPE_MOTION)
        return PRIORITY_MEDIUM;
    return PRIORITY_NONE;
}

static void parse_building_camera(const char* topic, char* building, size_t building_len,
    char* camera, size_t camera_len)
{
    if (building_len > 0)
        building[0] = '\0';
    if (camera_len > 0)
        camera[0] = '\0';

    if (!topic)
        return;

    size_t prefix_len = std::strlen(TOPIC_DATA_PREFIX);
    if (std::strncmp(topic, TOPIC_DATA_PREFIX, prefix_len) != 0)
        return;

    const char* rest = topic + prefix_len;
    const char* slash = std::strchr(rest, '/');

    if (!slash)
    {
        std::snprintf(building, building_len, "%s", rest);
        return;
    }

    size_t building_part_len = (size_t)(slash - rest);
    std::snprintf(building, building_len, "%.*s", (int)building_part_len, rest);
    std::snprintf(camera, camera_len, "%s", slash + 1);
}

static int get_latest_ct_version(EdgeContext* ctx)
{
    std::lock_guard<std::mutex> lock(ctx->ct_mutex);
    return ctx->has_latest_ct ? ctx->latest_ct.version : -1;
}

static void store_latest_ct(EdgeContext* ctx, const ConnectionTable& ct)
{
    std::lock_guard<std::mutex> lock(ctx->ct_mutex);
    ctx->latest_ct = ct;
    ctx->has_latest_ct = true;
}

// core / backup 공통 등록 함수
static void publish_edge_status(struct mosquitto* mosq, EdgeContext* ctx, const char* label)
{
    // Core에 자신 등록: description 필드에 "ip:port" 인코딩
    MqttMessage reg = {};
    uuid_generate(reg.msg_id);
    set_now_utc(reg.timestamp, sizeof(reg.timestamp));
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

// Forward declarations ===========================================
static void on_connect_backup(struct mosquitto* mosq, void* userdata, int rc);
static void on_disconnect_backup(struct mosquitto* mosq, void* userdata, int rc);
static void on_message_backup(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg);

static bool reconnect_backup_client(EdgeContext* ctx, const char* ip, uint16_t port);
static bool send_ping_to_node(EdgeContext* ctx, const NodeEntry& node);
static void start_rtt_probes_from_ct(EdgeContext* ctx, const ConnectionTable& ct);
static bool take_pending_ping_sent_at(EdgeContext* ctx, const char* node_id,
    std::chrono::steady_clock::time_point* out_sent_at);
static void upsert_measured_link_rtt(EdgeContext* ctx, const char* neighbor_id, float rtt_ms);
static void recompute_best_relay(EdgeContext* ctx);
static bool get_selected_relay(EdgeContext* ctx, char* out_node_id, size_t len);
static void build_relay_topic(const char* target_node_id, char* out_topic, size_t len);
static void build_data_topic_from_message(const MqttMessage& msg, char* out_topic, size_t len);
static bool deliver_event_message(EdgeContext* ctx, const char* topic, const MqttMessage& msg);
static void handle_prebuilt_event_delivery(EdgeContext* ctx, const char* topic, const MqttMessage& msg);
static void handle_relay_message(EdgeContext* ctx, const struct mosquitto_message* msg);
static void flush_store_queue(EdgeContext* ctx);

static bool parse_ip_port_description(const char* desc,
    char* out_ip, size_t out_ip_len, uint16_t* out_port)
{
    if (!desc || !out_ip || !out_port)
        return false;

    const char* colon = std::strrchr(desc, ':');
    if (!colon || colon == desc)
        return false;

    size_t ip_len = (size_t)(colon - desc);
    if (ip_len == 0 || ip_len >= out_ip_len)
        return false;

    char ip_buf[IP_LEN] = {};
    std::memcpy(ip_buf, desc, ip_len);
    ip_buf[ip_len] = '\0';

    char* end = nullptr;
    long port = std::strtol(colon + 1, &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535)
        return false;

    struct in_addr addr {};
    if (inet_pton(AF_INET, ip_buf, &addr) != 1)
        return false;

    std::strncpy(out_ip, ip_buf, out_ip_len - 1);
    out_ip[out_ip_len - 1] = '\0';
    *out_port = (uint16_t)port;
    return true;
}

static bool ensure_backup_client_exists(EdgeContext* ctx)
{
    if (ctx->mosq_backup)
        return true;

    char backup_client_id[64];
    std::snprintf(backup_client_id, sizeof(backup_client_id), "%s-backup", ctx->edge_id);

    struct mosquitto* mosq_backup = mosquitto_new(backup_client_id, true, ctx);
    if (!mosq_backup)
    {
        std::fprintf(stderr, "[edge] mosquitto_new for backup failed\n");
        return false;
    }

    MqttMessage lwt_backup = {};
    std::strncpy(lwt_backup.msg_id, ctx->edge_id, UUID_LEN - 1);
    lwt_backup.msg_id[UUID_LEN - 1] = '\0';
    lwt_backup.type = MSG_TYPE_LWT_NODE;
    lwt_backup.source.role = NODE_ROLE_NODE;
    std::strncpy(lwt_backup.source.id, ctx->edge_id, UUID_LEN - 1);
    lwt_backup.source.id[UUID_LEN - 1] = '\0';
    lwt_backup.delivery = { 1, false, false };

    char lwt_topic_backup[128];
    std::snprintf(lwt_topic_backup, sizeof(lwt_topic_backup), "%s%s",
        TOPIC_LWT_NODE_PREFIX, ctx->edge_id);

    std::string lwt_json_backup = mqtt_message_to_json(lwt_backup);
    mosquitto_will_set(mosq_backup, lwt_topic_backup,
        (int)lwt_json_backup.size(), lwt_json_backup.c_str(), 1, false);

    mosquitto_connect_callback_set(mosq_backup, on_connect_backup);
    mosquitto_message_callback_set(mosq_backup, on_message_backup);
    mosquitto_disconnect_callback_set(mosq_backup, on_disconnect_backup);
    mosquitto_reconnect_delay_set(mosq_backup, 2, 30, false);

    ctx->mosq_backup = mosq_backup;
    return true;
}

static bool reconnect_backup_client(EdgeContext* ctx, const char* ip, uint16_t port)
{
    if (!ensure_backup_client_exists(ctx))
        return false;

    const bool same_endpoint =
        ctx->backup_endpoint_known &&
        std::strncmp(ctx->backup_ip, ip, IP_LEN) == 0 &&
        ctx->backup_port == port;

    std::strncpy(ctx->backup_ip, ip, IP_LEN - 1);
    ctx->backup_ip[IP_LEN - 1] = '\0';
    ctx->backup_port = port;
    ctx->backup_endpoint_known = true;

    if (!ctx->backup_loop_started)
    {
        int loop_rc = mosquitto_loop_start(ctx->mosq_backup);
        if (loop_rc != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] backup loop_start failed: %s\n",
                mosquitto_strerror(loop_rc));
            return false;
        }
        ctx->backup_loop_started = true;
    }

    if (same_endpoint && ctx->backup_connected)
    {
        std::printf("[edge] backup endpoint already connected: %s:%u\n", ip, port);
        return true;
    }

    if (ctx->backup_connected)
    {
        mosquitto_disconnect(ctx->mosq_backup);
    }

    int rc = mosquitto_connect_async(ctx->mosq_backup, ctx->backup_ip, ctx->backup_port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] reconnect to backup core failed: %s\n",
            mosquitto_strerror(rc));
        return false;
    }

    std::printf("[edge] reconnecting backup core to %s:%u\n",
        ctx->backup_ip, ctx->backup_port);
    return true;
}

static bool send_ping_to_node(EdgeContext* ctx, const NodeEntry& node)
{
    if (node.role != NODE_ROLE_NODE)
        return false;
    if (node.status != NODE_STATUS_ONLINE)
        return false;
    if (std::strncmp(node.id, ctx->edge_id, UUID_LEN) == 0)
        return false;

    MqttMessage ping = {};
    uuid_generate(ping.msg_id);
    set_now_utc(ping.timestamp, sizeof(ping.timestamp));

    ping.type = MSG_TYPE_PING_REQUEST;
    ping.priority = PRIORITY_NONE;

    ping.source.role = NODE_ROLE_NODE;
    std::strncpy(ping.source.id, ctx->edge_id, UUID_LEN - 1);
    ping.source.id[UUID_LEN - 1] = '\0';

    ping.target.role = NODE_ROLE_NODE;
    std::strncpy(ping.target.id, node.id, UUID_LEN - 1);
    ping.target.id[UUID_LEN - 1] = '\0';

    std::strncpy(ping.route.original_node, ctx->edge_id, UUID_LEN - 1);
    ping.route.original_node[UUID_LEN - 1] = '\0';
    std::strncpy(ping.route.prev_hop, ctx->edge_id, UUID_LEN - 1);
    ping.route.prev_hop[UUID_LEN - 1] = '\0';
    std::strncpy(ping.route.next_hop, node.id, UUID_LEN - 1);
    ping.route.next_hop[UUID_LEN - 1] = '\0';
    ping.route.hop_count = 1;
    ping.route.ttl = 3;

    ping.delivery = { 0, false, false };
    std::strncpy(ping.payload.description, "RTT probe", DESCRIPTION_LEN - 1);
    ping.payload.description[DESCRIPTION_LEN - 1] = '\0';

    char ping_topic[128];
    std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, node.id);

    if (!forward_message_upstream(ctx, ping_topic, ping))
    {
        std::fprintf(stderr, "[edge] ping send failed: target=%s\n", node.id);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(ctx->ping_mutex);
        ctx->pending_ping_sent_at[std::string(node.id)] = std::chrono::steady_clock::now();
    }

    std::printf("[edge] ping sent: target=%s hop_to_core=%d\n", node.id, node.hop_to_core);
    return true;
}

static void start_rtt_probes_from_ct(EdgeContext* ctx, const ConnectionTable& ct)
{
    int sent_count = 0;

    for (int i = 0; i < ct.node_count; ++i)
    {
        if (send_ping_to_node(ctx, ct.nodes[i]))
            sent_count++;
    }

    std::printf("[edge] RTT probe round started: sent=%d\n", sent_count);
}

static bool take_pending_ping_sent_at(EdgeContext* ctx, const char* node_id,
    std::chrono::steady_clock::time_point* out_sent_at)
{
    if (!node_id || !out_sent_at)
        return false;

    std::lock_guard<std::mutex> lock(ctx->ping_mutex);
    auto it = ctx->pending_ping_sent_at.find(std::string(node_id));
    if (it == ctx->pending_ping_sent_at.end())
        return false;

    *out_sent_at = it->second;
    ctx->pending_ping_sent_at.erase(it);
    return true;
}

static void upsert_measured_link_rtt(EdgeContext* ctx, const char* neighbor_id, float rtt_ms)
{
    if (!neighbor_id || neighbor_id[0] == '\0')
        return;

    LinkEntry link = {};
    std::strncpy(link.from_id, ctx->edge_id, UUID_LEN - 1);
    link.from_id[UUID_LEN - 1] = '\0';
    std::strncpy(link.to_id, neighbor_id, UUID_LEN - 1);
    link.to_id[UUID_LEN - 1] = '\0';
    link.rtt_ms = rtt_ms;

    if (ctx->ct_manager)
    {
        ctx->ct_manager->addLink(link);
    }

    std::lock_guard<std::mutex> lock(ctx->ct_mutex);
    if (!ctx->has_latest_ct)
        return;

    for (int i = 0; i < ctx->latest_ct.link_count; ++i)
    {
        if (std::strncmp(ctx->latest_ct.links[i].from_id, ctx->edge_id, UUID_LEN) == 0 &&
            std::strncmp(ctx->latest_ct.links[i].to_id, neighbor_id, UUID_LEN) == 0)
        {
            ctx->latest_ct.links[i].rtt_ms = rtt_ms;
            return;
        }
    }

    if (ctx->latest_ct.link_count < MAX_LINKS)
    {
        ctx->latest_ct.links[ctx->latest_ct.link_count++] = link;
    }
}

static void recompute_best_relay(EdgeContext* ctx)
{
    ConnectionTable ct = {};
    {
        std::lock_guard<std::mutex> lock(ctx->ct_mutex);
        if (!ctx->has_latest_ct)
        {
            std::lock_guard<std::mutex> relay_lock(ctx->relay_mutex);
            ctx->relay_selected = false;
            ctx->relay_node_id[0] = '\0';
            ctx->relay_rtt_ms = -1.0f;
            ctx->relay_hop_to_core = -1;
            return;
        }
        ct = ctx->latest_ct;
    }

    bool found = false;
    NodeEntry best_node = {};
    float best_rtt = 0.0f;

    for (int i = 0; i < ct.node_count; ++i)
    {
        const NodeEntry& node = ct.nodes[i];
        if (node.role != NODE_ROLE_NODE)
            continue;
        if (node.status != NODE_STATUS_ONLINE)
            continue;
        if (std::strncmp(node.id, ctx->edge_id, UUID_LEN) == 0)
            continue;

        float measured_rtt = -1.0f;
        for (int j = 0; j < ct.link_count; ++j)
        {
            const LinkEntry& link = ct.links[j];
            if (std::strncmp(link.from_id, ctx->edge_id, UUID_LEN) == 0 &&
                std::strncmp(link.to_id, node.id, UUID_LEN) == 0 &&
                link.rtt_ms > 0.0f)
            {
                measured_rtt = link.rtt_ms;
                break;
            }
        }

        if (measured_rtt <= 0.0f)
            continue;

        if (!found)
        {
            found = true;
            best_node = node;
            best_rtt = measured_rtt;
            continue;
        }

        const bool better_rtt = measured_rtt < best_rtt;
        const bool same_rtt = std::fabs(measured_rtt - best_rtt) < 0.001f;
        const bool better_hop = same_rtt && node.hop_to_core < best_node.hop_to_core;

        if (better_rtt || better_hop)
        {
            best_node = node;
            best_rtt = measured_rtt;
        }
    }

    std::lock_guard<std::mutex> relay_lock(ctx->relay_mutex);
    if (!found)
    {
        bool had_old = ctx->relay_selected;
        ctx->relay_selected = false;
        ctx->relay_node_id[0] = '\0';
        ctx->relay_rtt_ms = -1.0f;
        ctx->relay_hop_to_core = -1;

        if (had_old)
            std::printf("[edge] relay cleared: no eligible node\n");
        return;
    }

    bool changed =
        !ctx->relay_selected ||
        std::strncmp(ctx->relay_node_id, best_node.id, UUID_LEN) != 0 ||
        std::fabs(ctx->relay_rtt_ms - best_rtt) >= 0.001f ||
        ctx->relay_hop_to_core != best_node.hop_to_core;

    ctx->relay_selected = true;
    std::strncpy(ctx->relay_node_id, best_node.id, UUID_LEN - 1);
    ctx->relay_node_id[UUID_LEN - 1] = '\0';
    ctx->relay_rtt_ms = best_rtt;
    ctx->relay_hop_to_core = best_node.hop_to_core;

    if (changed)
    {
        std::printf("[edge] best relay selected: node=%s rtt=%.2f ms hop=%d\n",
            ctx->relay_node_id, ctx->relay_rtt_ms, ctx->relay_hop_to_core);
    }
}

static bool get_selected_relay(EdgeContext* ctx, char* out_node_id, size_t len)
{
    if (!out_node_id || len == 0)
        return false;

    std::lock_guard<std::mutex> lock(ctx->relay_mutex);
    if (!ctx->relay_selected || ctx->relay_node_id[0] == '\0')
        return false;

    std::strncpy(out_node_id, ctx->relay_node_id, len - 1);
    out_node_id[len - 1] = '\0';
    return true;
}

static void build_relay_topic(const char* target_node_id, char* out_topic, size_t len)
{
    if (!out_topic || len == 0)
        return;

    std::snprintf(out_topic, len, "%s%s", TOPIC_RELAY_PREFIX,
        target_node_id ? target_node_id : "");
}

static void build_data_topic_from_message(const MqttMessage& msg, char* out_topic, size_t len)
{
    if (!out_topic || len == 0)
        return;

    if (msg.payload.building_id[0] != '\0' && msg.payload.camera_id[0] != '\0')
    {
        std::snprintf(out_topic, len, "%s%s/%s",
            TOPIC_DATA_PREFIX,
            msg.payload.building_id,
            msg.payload.camera_id);
        return;
    }

    if (msg.payload.building_id[0] != '\0')
    {
        std::snprintf(out_topic, len, "%s%s",
            TOPIC_DATA_PREFIX,
            msg.payload.building_id);
        return;
    }

    std::snprintf(out_topic, len, "%srelay", TOPIC_DATA_PREFIX);
}

static bool deliver_event_message(EdgeContext* ctx, const char* topic, const MqttMessage& msg)
{
    const bool is_data_topic =
        topic &&
        std::strncmp(topic, TOPIC_DATA_PREFIX, std::strlen(TOPIC_DATA_PREFIX)) == 0;

    const bool originated_here =
        std::strncmp(msg.route.original_node, ctx->edge_id, UUID_LEN) == 0;

    // 원본 publisher인 경우에만 relay 우선 시도
    if (is_data_topic && originated_here && msg.route.ttl > 1)
    {
        char relay_id[UUID_LEN];
        if (get_selected_relay(ctx, relay_id, sizeof(relay_id)))
        {
            MqttMessage relay_msg = msg;
            relay_msg.target.role = NODE_ROLE_NODE;
            std::strncpy(relay_msg.target.id, relay_id, UUID_LEN - 1);
            relay_msg.target.id[UUID_LEN - 1] = '\0';

            std::strncpy(relay_msg.route.prev_hop, ctx->edge_id, UUID_LEN - 1);
            relay_msg.route.prev_hop[UUID_LEN - 1] = '\0';
            std::strncpy(relay_msg.route.next_hop, relay_id, UUID_LEN - 1);
            relay_msg.route.next_hop[UUID_LEN - 1] = '\0';
            relay_msg.route.hop_count += 1;
            relay_msg.route.ttl -= 1;

            char relay_topic[128];
            build_relay_topic(relay_id, relay_topic, sizeof(relay_topic));

            if (forward_message_upstream(ctx, relay_topic, relay_msg))
            {
                std::printf("[edge] delivered via relay\n");
                std::printf("  relay    : %s\n", relay_id);
                std::printf("  msg_id   : %s\n", relay_msg.msg_id);
                return true;
            }
        }
    }

    return forward_message_upstream(ctx, topic, msg);
}

static void handle_prebuilt_event_delivery(EdgeContext* ctx, const char* topic, const MqttMessage& msg)
{
    if (has_pending_queue(ctx))
    {
        flush_store_queue(ctx);

        if (has_pending_queue(ctx))
        {
            queue_event(ctx, topic, msg);
            return;
        }
    }

    if (deliver_event_message(ctx, topic, msg))
        return;

    queue_event(ctx, topic, msg);
}

static void handle_relay_message(EdgeContext* ctx, const struct mosquitto_message* msg)
{
    MqttMessage relay_msg = {};
    std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
    if (!mqtt_message_from_json(json, relay_msg))
        return;

    const char* target_id = msg->topic + std::strlen(TOPIC_RELAY_PREFIX);
    if (!target_id || target_id[0] == '\0')
        return;

    if (std::strncmp(target_id, ctx->edge_id, UUID_LEN) != 0)
        return;

    if (relay_msg.target.role != NODE_ROLE_NODE ||
        std::strncmp(relay_msg.target.id, ctx->edge_id, UUID_LEN) != 0)
    {
        std::printf("[edge] relay ignored: target mismatch\n");
        return;
    }

    if (relay_msg.route.ttl <= 0)
    {
        std::printf("[edge] relay dropped: ttl exhausted\n");
        return;
    }

    char inbound_prev_hop[UUID_LEN] = {};
    std::strncpy(inbound_prev_hop, relay_msg.route.prev_hop, UUID_LEN - 1);

    MqttMessage forwarded = relay_msg;
    forwarded.target.role = NODE_ROLE_CORE;
    forwarded.target.id[0] = '\0';

    std::strncpy(forwarded.route.prev_hop, ctx->edge_id, UUID_LEN - 1);
    forwarded.route.prev_hop[UUID_LEN - 1] = '\0';
    forwarded.route.next_hop[0] = '\0';
    forwarded.route.hop_count += 1;
    forwarded.route.ttl -= 1;

    char data_topic[128];
    build_data_topic_from_message(forwarded, data_topic, sizeof(data_topic));

    std::printf("[edge] relay received\n");
    std::printf("  from     : %s\n", inbound_prev_hop);
    std::printf("  msg_id   : %s\n", relay_msg.msg_id);

    handle_prebuilt_event_delivery(ctx, data_topic, forwarded);
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

        if (!deliver_event_message(ctx, item.topic, item.msg))
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
    handle_prebuilt_event_delivery(ctx, topic, event_msg);
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

    char ping_topic[128];
    char pong_topic[128];
    char relay_topic[128];

    std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
    std::snprintf(pong_topic, sizeof(pong_topic), "%s%s", TOPIC_PONG_PREFIX, ctx->edge_id);
    std::snprintf(relay_topic, sizeof(relay_topic), "%s%s", TOPIC_RELAY_PREFIX, ctx->edge_id);

    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_SWITCH, 1);
    mosquitto_subscribe(mosq, nullptr, ping_topic, 0);
    mosquitto_subscribe(mosq, nullptr, pong_topic, 0);
    mosquitto_subscribe(mosq, nullptr, relay_topic, 1);

    publish_edge_status(mosq, ctx, "core");
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

    // CT 수신: version 비교 후 최신 CT만 반영
    if (std::strcmp(msg->topic, TOPIC_TOPOLOGY) == 0)
    {
        ConnectionTable ct = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, ct))
            return;

        const int current_version = get_latest_ct_version(ctx);
        if (current_version >= 0 && ct.version <= current_version)
        {
            std::printf("[edge] ignored stale CT (incoming=%d, current=%d)\n",
                ct.version, current_version);
            return;
        }

        store_latest_ct(ctx, ct);

        std::printf("[edge] CT updated (old=%d, new=%d, nodes=%d, links=%d)\n",
            current_version, ct.version, ct.node_count, ct.link_count);

        start_rtt_probes_from_ct(ctx, ct);
        recompute_best_relay(ctx);
        return;
    }

    // 명시적 core_switch 수신: backup endpoint로 재연결
    if (std::strcmp(msg->topic, TOPIC_CORE_SWITCH) == 0)
    {
        MqttMessage sw = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, sw))
            return;

        char new_backup_ip[IP_LEN] = {};
        uint16_t new_backup_port = 0;
        if (!parse_ip_port_description(sw.payload.description,
                new_backup_ip, sizeof(new_backup_ip), &new_backup_port))
        {
            std::fprintf(stderr, "[edge] core_switch ignored: invalid endpoint '%s'\n",
                sw.payload.description);
            return;
        }

        std::printf("[edge] core_switch received: %s:%u\n", new_backup_ip, new_backup_port);
        ctx->prefer_backup = true;
        ctx->core_connected = false;

        if (reconnect_backup_client(ctx, new_backup_ip, new_backup_port))
        {
            if (ctx->backup_connected && ctx->mosq_backup)
                flush_store_queue(ctx);
        }
        return;
    }

    // Core LWT 수신: backup core를 사실상 1순위로 승격
    if (std::strncmp(msg->topic, TOPIC_LWT_CORE_PREFIX, std::strlen(TOPIC_LWT_CORE_PREFIX)) == 0)
    {
        std::printf("[edge] core down: %s\n", msg->topic + std::strlen(TOPIC_LWT_CORE_PREFIX));
        ctx->core_connected = false;
        ctx->prefer_backup = true;
        std::printf("[edge] switched to backup-preferred mode\n");

        if (ctx->backup_connected && ctx->mosq_backup)
            flush_store_queue(ctx);
        return;
    }

    // Relay 수신 → Core로 재전달
    if (std::strncmp(msg->topic, TOPIC_RELAY_PREFIX, std::strlen(TOPIC_RELAY_PREFIX)) == 0)
    {
        handle_relay_message(ctx, msg);
        return;
    }

    // Pong 수신 → RTT 계산 + link RTT 갱신
    if (std::strncmp(msg->topic, TOPIC_PONG_PREFIX, std::strlen(TOPIC_PONG_PREFIX)) == 0)
    {
        MqttMessage pong = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, pong))
            return;
        if (pong.type != MSG_TYPE_PING_RESPONSE)
            return;

        const char* responder_id = pong.source.id;
        if (!responder_id || responder_id[0] == '\0')
            return;

        std::chrono::steady_clock::time_point sent_at;
        if (!take_pending_ping_sent_at(ctx, responder_id, &sent_at))
        {
            std::printf("[edge] pong ignored: no pending ping for %s\n", responder_id);
            return;
        }

        auto now = std::chrono::steady_clock::now();
        float rtt_ms = std::chrono::duration<float, std::milli>(now - sent_at).count();

        upsert_measured_link_rtt(ctx, responder_id, rtt_ms);
        recompute_best_relay(ctx);

        std::printf("[edge] pong received: from=%s rtt=%.2f ms\n", responder_id, rtt_ms);
        return;
    }

    // Ping 수신 → Pong 응답
    if (std::strncmp(msg->topic, TOPIC_PING_PREFIX, std::strlen(TOPIC_PING_PREFIX)) == 0)
    {
        MqttMessage ping = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, ping))
            return;

        MqttMessage pong = {};
        uuid_generate(pong.msg_id);
        set_now_utc(pong.timestamp, sizeof(pong.timestamp));
        pong.type = MSG_TYPE_PING_RESPONSE;
        pong.source.role = NODE_ROLE_NODE;
        std::strncpy(pong.source.id, ctx->edge_id, UUID_LEN - 1);
        pong.source.id[UUID_LEN - 1] = '\0';

        pong.target.role = NODE_ROLE_NODE;
        std::strncpy(pong.target.id, ping.source.id, UUID_LEN - 1);
        pong.target.id[UUID_LEN - 1] = '\0';

        std::strncpy(pong.route.original_node, ctx->edge_id, UUID_LEN - 1);
        pong.route.original_node[UUID_LEN - 1] = '\0';
        std::strncpy(pong.route.prev_hop, ctx->edge_id, UUID_LEN - 1);
        pong.route.prev_hop[UUID_LEN - 1] = '\0';
        std::strncpy(pong.route.next_hop, ping.source.id, UUID_LEN - 1);
        pong.route.next_hop[UUID_LEN - 1] = '\0';
        pong.route.hop_count = 1;
        pong.route.ttl = 3;

        pong.delivery = { 0, false, false };

        char pong_topic[128];
        std::snprintf(pong_topic, sizeof(pong_topic), "%s%s", TOPIC_PONG_PREFIX, ping.source.id);

        std::string pong_json = mqtt_message_to_json(pong);
        mosquitto_publish(mosq, nullptr, pong_topic,
            (int)pong_json.size(), pong_json.c_str(), 0, false);
        return;
    }
}

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
    std::snprintf(topic, sizeof(topic), "%s#", TOPIC_DATA_PREFIX);

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

    if (std::strncmp(msg->topic, TOPIC_DATA_PREFIX, std::strlen(TOPIC_DATA_PREFIX)) != 0)
        return;

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

    char ping_topic[128];
    char pong_topic[128];
    char relay_topic[128];

    std::snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
    std::snprintf(pong_topic, sizeof(pong_topic), "%s%s", TOPIC_PONG_PREFIX, ctx->edge_id);
    std::snprintf(relay_topic, sizeof(relay_topic), "%s%s", TOPIC_RELAY_PREFIX, ctx->edge_id);

    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_SWITCH, 1);
    mosquitto_subscribe(mosq, nullptr, ping_topic, 0);
    mosquitto_subscribe(mosq, nullptr, pong_topic, 0);
    mosquitto_subscribe(mosq, nullptr, relay_topic, 1);

    publish_edge_status(mosq, ctx, "backup core");
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
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* broker_host = argv[1];
    int broker_port = std::atoi(argv[2]);
    const char* core_ip = argv[3];
    int core_port = std::atoi(argv[4]);
    const char* backup_core_ip = (argc > 5) ? argv[5] : "";
    int backup_port = (argc > 6) ? std::atoi(argv[6]) : 1883;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    EdgeContext ctx{};
    uuid_generate(ctx.edge_id);
    ctx.node_port = (uint16_t)broker_port;
    ctx.mosq_core = nullptr;
    ctx.mosq_backup = nullptr;
    ctx.core_connected = false;
    ctx.backup_connected = false;
    ctx.prefer_backup = false;

    ctx.has_latest_ct = false;
    std::memset(&ctx.latest_ct, 0, sizeof(ctx.latest_ct));

    ctx.backup_ip[0] = '\0';
    ctx.backup_port = 0;
    ctx.backup_endpoint_known = false;
    ctx.backup_loop_started = false;

    if (backup_core_ip[0] != '\0')
    {
        std::strncpy(ctx.backup_ip, backup_core_ip, IP_LEN - 1);
        ctx.backup_ip[IP_LEN - 1] = '\0';
        ctx.backup_port = (uint16_t)backup_port;
        ctx.backup_endpoint_known = true;
    }

    ctx.relay_node_id[0] = '\0';
    ctx.relay_selected = false;
    ctx.relay_rtt_ms = -1.0f;
    ctx.relay_hop_to_core = -1;

    if (!get_outbound_ip(core_ip, core_port, ctx.node_ip, sizeof(ctx.node_ip)))
    {
        std::fprintf(stderr, "[edge] failed to detect outbound IP toward %s\n", core_ip);
        return 1;
    }

    ConnectionTableManager ct_manager;
    ctx.ct_manager = &ct_manager;
    ct_manager.init("", "");

    mosquitto_lib_init();

    struct mosquitto* mosq_core = mosquitto_new(ctx.edge_id, true, &ctx);
    if (!mosq_core)
    {
        std::fprintf(stderr, "[edge] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    ctx.mosq_core = mosq_core;

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
        std::snprintf(lwt_topic, sizeof(lwt_topic), "%s%s", TOPIC_LWT_NODE_PREFIX, ctx.edge_id);

        std::string lwt_json = mqtt_message_to_json(lwt);
        mosquitto_will_set(mosq_core, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

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

    mosquitto_reconnect_delay_set(mosq_core, 2, 30, false);
    int rc = mosquitto_connect(mosq_core, core_ip, core_port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] connect to core failed: %s\n", mosquitto_strerror(rc));
        if (mosq_backup)
            mosquitto_destroy(mosq_backup);
        mosquitto_destroy(mosq_local);
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_reconnect_delay_set(mosq_local, 2, 30, false);
    int rc_local = mosquitto_connect(mosq_local, broker_host, broker_port, 60);
    if (rc_local != MOSQ_ERR_SUCCESS)
    {
        std::fprintf(stderr, "[edge] connect to local broker failed: %s\n", mosquitto_strerror(rc_local));
        if (mosq_backup)
            mosquitto_destroy(mosq_backup);
        mosquitto_destroy(mosq_local);
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    if (mosq_backup)
    {
        mosquitto_reconnect_delay_set(mosq_backup, 2, 30, false);
        int rc_backup = mosquitto_connect(mosq_backup, ctx.backup_ip, ctx.backup_port, 60);
        if (rc_backup != MOSQ_ERR_SUCCESS)
        {
            std::fprintf(stderr, "[edge] connect to backup core failed: %s\n",
                mosquitto_strerror(rc_backup));
            mosquitto_destroy(mosq_backup);
            mosq_backup = nullptr;
            ctx.mosq_backup = nullptr;
            ctx.backup_connected = false;
            ctx.backup_loop_started = false;
        }
    }

    mosquitto_loop_start(mosq_core);
    mosquitto_loop_start(mosq_local);
    if (mosq_backup)
    {
        mosquitto_loop_start(mosq_backup);
        ctx.backup_loop_started = true;
    }

    std::printf("[edge] %s  local=%s:%d  core=%s:%d  backup=%s\n",
        ctx.edge_id, ctx.node_ip, broker_port,
        core_ip, core_port,
        backup_core_ip[0] ? backup_core_ip : "(none)");

    while (g_running)
    {
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
