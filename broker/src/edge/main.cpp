#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"

// constant TOPICs =====================================================
#define TOPIC_TOPOLOGY         "campus/monitor/topology"
#define TOPIC_CORE_WILL_ALL    "campus/will/core/#"
#define TOPIC_LWT_NODE_PREFIX  "campus/will/node/"
#define TOPIC_STATUS_PREFIX    "campus/monitor/status/"
#define TOPIC_PING_PREFIX      "campus/monitor/ping/"
#define TOPIC_PONG_PREFIX      "campus/monitor/pong/"

// Global State =====================================================
static volatile bool g_running = true;

struct EdgeContext {
    char                    edge_id[UUID_LEN];
    char                    node_ip[IP_LEN];
    uint16_t                node_port;
    ConnectionTableManager* ct_manager;
};

static void handle_signal(int) { g_running = false; }

// core 방향 outbound IP 감지 (실제 패킷 전송 없음)
static bool get_outbound_ip(const char* dest_ip, int dest_port, char* out, size_t len) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)dest_port);
    dest.sin_addr.s_addr = inet_addr(dest_ip);

    if (connect(sock, (sockaddr*)&dest, sizeof(dest)) < 0) {
        close(sock);
        return false;
    }

    sockaddr_in local{};
    socklen_t   local_len = sizeof(local);
    getsockname(sock, (sockaddr*)&local, &local_len);
    close(sock);

    inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)len);
    return true;
}

// Callbacks =====================================================

static void on_connect_core(struct mosquitto* mosq, void* userdata, int rc) {
    if (rc != 0) {
        fprintf(stderr, "[edge] core connect failed (rc=%d)\n", rc);
        return;
    }
    printf("[edge] connected to core\n");

    auto* ctx = static_cast<EdgeContext*>(userdata);

    // 구독
    char ping_topic[128];
    snprintf(ping_topic, sizeof(ping_topic), "%s%s", TOPIC_PING_PREFIX, ctx->edge_id);
    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, ping_topic, 0);

    // Core에 자신 등록: description 필드에 "ip:port" 인코딩
    MqttMessage reg = {};
    uuid_generate(reg.msg_id);
    reg.type = MSG_TYPE_STATUS;
    reg.source.role = NODE_ROLE_NODE;
    strncpy(reg.source.id, ctx->edge_id, UUID_LEN - 1);
    reg.target.role = NODE_ROLE_CORE;
    reg.delivery = { 1, false, false };
    snprintf(reg.payload.description, DESCRIPTION_LEN,
        "%s:%u", ctx->node_ip, ctx->node_port);

    char status_topic[128];
    snprintf(status_topic, sizeof(status_topic), "%s%s",
        TOPIC_STATUS_PREFIX, ctx->edge_id);

    std::string json = mqtt_message_to_json(reg);
    mosquitto_publish(mosq, nullptr, status_topic,
        (int)json.size(), json.c_str(), 1, false);

    printf("[edge] registered: id=%s  ip=%s:%u\n",
        ctx->edge_id, ctx->node_ip, ctx->node_port);
}

static void on_disconnect_core(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[edge] disconnected from core (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_core(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg) {
    auto* ctx = static_cast<EdgeContext*>(userdata);

    // CT 수신 (M-04): 로컬 CT 갱신
    if (strcmp(msg->topic, TOPIC_TOPOLOGY) == 0) {
        ConnectionTable ct;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (connection_table_from_json(json, ct)) {
            printf("[edge] CT received (version=%d, nodes=%d)\n",
                ct.version, ct.node_count);
            // TODO: version 비교 후 구버전 무시, RTT 측정 → relay 경로 초기화
        }
        return;
    }

    // Core LWT 수신 (W-01): backup core로 재연결
    if (strncmp(msg->topic, "campus/will/core/", 17) == 0) {
        printf("[edge] core down: %s\n", msg->topic + 17);
        // TODO: LWT payload에서 backup core IP:port 파싱 후 재연결
        return;
    }

    // Ping 수신 → Pong 응답 (M-01 / M-02)
    if (strncmp(msg->topic, TOPIC_PING_PREFIX, strlen(TOPIC_PING_PREFIX)) == 0) {
        MqttMessage ping;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, ping)) return;

        MqttMessage pong = {};
        uuid_generate(pong.msg_id);
        pong.type = MSG_TYPE_PING_RESPONSE;
        pong.source.role = NODE_ROLE_NODE;
        strncpy(pong.source.id, ctx->edge_id, UUID_LEN - 1);
        pong.target.role = NODE_ROLE_NODE;
        strncpy(pong.target.id, ping.source.id, UUID_LEN - 1);
        pong.delivery = { 0, false, false };

        char pong_topic[128];
        snprintf(pong_topic, sizeof(pong_topic), "%s%s",
            TOPIC_PONG_PREFIX, ping.source.id);

        std::string pong_json = mqtt_message_to_json(pong);
        mosquitto_publish(mosq, nullptr, pong_topic,
            (int)pong_json.size(), pong_json.c_str(), 0, false);
        return;
    }
}

// main =====================================================

int main(int argc, char* argv[]) {
    // 인수: <broker_host> <broker_port> <core_ip> <core_port> [backup_core_ip] [backup_core_port]
    if (argc < 5) {
        fprintf(stderr,
            "usage: %s <broker_host> <broker_port> <core_ip> <core_port>"
            " [backup_core_ip] [backup_core_port]\n", argv[0]);
        return 1;
    }
    const char* broker_host = argv[1];
    int         broker_port = atoi(argv[2]);
    const char* core_ip = argv[3];
    int         core_port = atoi(argv[4]);
    const char* backup_core_ip = (argc > 5) ? argv[5] : "";
    int         backup_port = (argc > 6) ? atoi(argv[6]) : 1883;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. edge_id UUID 생성
    EdgeContext ctx = {};
    uuid_generate(ctx.edge_id);
    ctx.node_port = (uint16_t)broker_port;  // 인접 노드들이 접속할 로컬 포트

    // 2. core 방향 outbound IP 자동 감지
    if (!get_outbound_ip(core_ip, core_port, ctx.node_ip, sizeof(ctx.node_ip))) {
        fprintf(stderr, "[edge] failed to detect outbound IP toward %s\n", core_ip);
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
    if (!mosq_core) {
        fprintf(stderr, "[edge] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    // 6. LWT 설정 (W-02): 비정상 종료 시 core가 OFFLINE 처리
    {
        MqttMessage lwt = {};
        strncpy(lwt.msg_id, ctx.edge_id, UUID_LEN - 1);
        lwt.type = MSG_TYPE_LWT_NODE;
        lwt.source.role = NODE_ROLE_NODE;
        strncpy(lwt.source.id, ctx.edge_id, UUID_LEN - 1);
        lwt.delivery = { 1, false, false };

        char lwt_topic[128];
        snprintf(lwt_topic, sizeof(lwt_topic), "%s%s",
            TOPIC_LWT_NODE_PREFIX, ctx.edge_id);

        std::string lwt_json = mqtt_message_to_json(lwt);
        mosquitto_will_set(mosq_core, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // 7. 콜백 등록
    mosquitto_connect_callback_set(mosq_core, on_connect_core);
    mosquitto_message_callback_set(mosq_core, on_message_core);
    mosquitto_disconnect_callback_set(mosq_core, on_disconnect_core);

    // 8. Core 브로커 연결 (auto-reconnect 활성화)
    mosquitto_reconnect_delay_set(mosq_core, 2, 30, false);
    int rc = mosquitto_connect(mosq_core, core_ip, core_port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[edge] connect to core failed: %s\n",
            mosquitto_strerror(rc));
        mosquitto_destroy(mosq_core);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 9. 이벤트 루프 시작
    mosquitto_loop_start(mosq_core);

    printf("[edge] %s  local=%s:%d  core=%s:%d  backup=%s\n",
        ctx.edge_id, ctx.node_ip, broker_port,
        core_ip, core_port,
        backup_core_ip[0] ? backup_core_ip : "(none)");

    // TODO: mosq_local (broker_host:broker_port) 연결 — 로컬 CCTV 이벤트 수집
    // TODO: mosq_backup (backup_core_ip:backup_port) 연결 — Backup Core 유지
    (void)broker_host;
    (void)backup_core_ip;
    (void)backup_port;

    while (g_running) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, nullptr);
    }

    printf("[edge] shutting down\n");
    mosquitto_loop_stop(mosq_core, true);
    mosquitto_destroy(mosq_core);
    mosquitto_lib_cleanup();
    return 0;
}
