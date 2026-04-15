#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"

// constant TOPICs 
#define TOPIC_LWT_CORE_PREFIX  "campus/will/core/"
#define TOPIC_TOPOLOGY         "campus/monitor/topology"
#define TOPIC_DATA_ALL         "campus/data/#"
#define TOPIC_RELAY_ALL        "campus/relay/#"
#define TOPIC_NODE_STATUS_ALL  "campus/monitor/status/#"
#define TOPIC_NODE_WILL_ALL    "campus/will/node/#"
#define TOPIC_CORE_WILL_ALL    "campus/will/core/#"
#define TOPIC_CT_SYNC          "_core/sync/connection_table"
#define TOPIC_ELECTION_ALL     "_core/election/#"

// Global State =====================================================
static volatile bool g_running = true;

struct CoreContext {
    const char* core_id;
    ConnectionTableManager* ct_manager;
};

static void handle_signal(int) { g_running = false; }

// Callbacks =====================================================

static void on_connect(struct mosquitto* mosq, void* userdata, int rc) {
    if (rc != 0) {
        fprintf(stderr, "[core] connect failed (rc=%d)\n", rc);
        return;
    }
    printf("[core] connected\n");

    // 토픽 구독
    mosquitto_subscribe(mosq, nullptr, TOPIC_DATA_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_RELAY_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_STATUS_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CT_SYNC, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_ELECTION_ALL, 1);

    // 초기 ConnectionTable 스냅샷을 Retained Message로 게시
    auto* ctx = static_cast<CoreContext*>(userdata);
    ConnectionTable ct = ctx->ct_manager->snapshot();
    std::string json = connection_table_to_json(ct);
    mosquitto_publish(mosq, nullptr, TOPIC_TOPOLOGY,
        (int)json.size(), json.c_str(), 1, /*retain=*/true);
    printf("[core] topology published (version=%d)\n", ct.version);
}

static void on_disconnect(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[core] disconnected (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg) {
    auto* ctx = static_cast<CoreContext*>(userdata);

    // Node 비정상 종료 LWT (W-02): OFFLINE 마킹 후 CT 브로드캐스트
    if (strncmp(msg->topic, "campus/will/node/", 17) == 0) {
        const char* node_id = msg->topic + 17;
        if (ctx->ct_manager->setNodeStatus(node_id, NODE_STATUS_OFFLINE)) {
            ConnectionTable ct = ctx->ct_manager->snapshot();
            std::string json = connection_table_to_json(ct);
            mosquitto_publish(mosq, nullptr, TOPIC_TOPOLOGY,
                (int)json.size(), json.c_str(), 1, true);
            printf("[core] node offline: %s  (ct.version=%d)\n", node_id, ct.version);
        }
        return;
    }

    // Core 간 CT 동기화 (C-01) — TODO: peer CT merge
    if (strcmp(msg->topic, TOPIC_CT_SYNC) == 0) {
        return;
    }

    // 이벤트 데이터 / Relay — TODO: msg_id 중복 필터링 후 Client 전달
}

// main =====================================================

int main(int argc, char* argv[]) {
    // 인수: <broker_host> <broker_port> [backup_core_id] [backup_ip] [backup_port]
    const char* broker_host    = (argc > 1) ? argv[1] : "127.0.0.1";
    int         broker_port    = (argc > 2) ? atoi(argv[2]) : 1883;
    const char* backup_core_id = (argc > 3) ? argv[3] : "";
    const char* backup_ip      = (argc > 4) ? argv[4] : "";
    int         backup_port    = (argc > 5) ? atoi(argv[5]) : 1883;

    // core_id: 기동 시 UUID 생성
    char core_id[UUID_LEN];
    uuid_generate(core_id);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. mosquitto 라이브러리 초기화
    mosquitto_lib_init();

    // 2. ConnectionTable 초기화 및 self(Core) 등록
    //    on_connect에서 CT를 바로 게시하므로 connect 전에 준비
    ConnectionTableManager ct_manager;
    ct_manager.init(core_id, backup_core_id);
    {
        NodeEntry self = {};
        strncpy(self.id, core_id, UUID_LEN - 1);
        self.role = NODE_ROLE_CORE;
        strncpy(self.ip, broker_host, IP_LEN - 1);
        self.port = (uint16_t)broker_port;
        self.status = NODE_STATUS_ONLINE;
        self.hop_to_core = 0;
        ct_manager.addNode(self);
    }

    // 3. mosquitto 클라이언트 생성
    CoreContext ctx = { core_id, &ct_manager };
    struct mosquitto* mosq = mosquitto_new(core_id, true, &ctx);
    if (!mosq) {
        fprintf(stderr, "[core] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    // 4. LWT 설정 (W-01): 비정상 종료 시 backup core IP:port 전파
    {
        MqttMessage lwt = {};
        strncpy(lwt.msg_id, core_id, UUID_LEN - 1);
        lwt.type = MSG_TYPE_LWT_CORE;
        lwt.source.role = NODE_ROLE_CORE;
        strncpy(lwt.source.id, core_id, UUID_LEN - 1);
        lwt.target.role = NODE_ROLE_CORE;
        strncpy(lwt.target.id, backup_core_id, UUID_LEN - 1);
        snprintf(lwt.payload.description, DESCRIPTION_LEN, "%s:%d", backup_ip, backup_port);

        char lwt_topic[128];
        snprintf(lwt_topic, sizeof(lwt_topic), "%s%s", TOPIC_LWT_CORE_PREFIX, core_id);

        std::string lwt_json = mqtt_message_to_json(lwt);
        mosquitto_will_set(mosq, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // 5. 콜백 등록
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    // 6. 브로커 연결 (auto-reconnect 활성화)
    mosquitto_reconnect_delay_set(mosq, 2, 30, false);
    int rc = mosquitto_connect(mosq, broker_host, broker_port, /*keepalive=*/60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[core] mosquitto_connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 7. 이벤트 루프 시작 (별도 스레드)
    mosquitto_loop_start(mosq);

    printf("[core] %s running on %s:%d  (backup: %s)\n",
        core_id, broker_host, broker_port,
        backup_core_id[0] ? backup_core_id : "(none)");

    while (g_running) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, nullptr);
    }

    // 8. 정상 종료
    printf("[core] shutting down\n");
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
