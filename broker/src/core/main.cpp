#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <unordered_set>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"
#include "core_helpers.h"

// Global State =====================================================
static volatile bool g_running = true;

struct CoreContext {
    char                            core_id[UUID_LEN];
    char                            core_ip[IP_LEN];      // own broker IP
    int                             core_port;             // own broker port
    bool                            is_backup;
    char                            active_core_ip[IP_LEN]; // Backup only: peer IP
    int                             active_core_port;        // Backup only: peer port
    ConnectionTableManager*         ct_manager;
    std::unordered_set<std::string> seen_msg_ids;
    struct mosquitto*               mosq_self;  // own broker connection
    struct mosquitto*               mosq_peer;  // Backup → Active's broker
};

static void handle_signal(int) { g_running = false; }

// CT 발행 헬퍼 =====================================================

// 자신 브로커의 TOPIC_TOPOLOGY 에 최신 CT 발행
static void publish_topology(struct mosquitto* mosq, CoreContext* ctx) {
    ConnectionTable ct = ctx->ct_manager->snapshot();
    std::string json = connection_table_to_json(ct);
    mosquitto_publish(mosq, nullptr, TOPIC_TOPOLOGY,
        (int)json.size(), json.c_str(), 1, true);
    printf("[core] topology published (version=%d, nodes=%d)\n", ct.version, ct.node_count);
}

// Active: TOPIC_CT_SYNC (retained) publish → Backup 수신
static void publish_ct_sync(struct mosquitto* mosq, CoreContext* ctx) {
    ConnectionTable ct = ctx->ct_manager->snapshot();
    std::string json = connection_table_to_json(ct);
    mosquitto_publish(mosq, nullptr, TOPIC_CT_SYNC,
        (int)json.size(), json.c_str(), 1, true);
}

// Backup: TOPIC_NODE_REGISTER publish → Active 수신
static void publish_node_register(CoreContext* ctx) {
    if (!ctx->mosq_peer) return;
    ConnectionTable ct = ctx->ct_manager->snapshot();
    std::string json = connection_table_to_json(ct);
    mosquitto_publish(ctx->mosq_peer, nullptr, TOPIC_NODE_REGISTER,
        (int)json.size(), json.c_str(), 1, false);
    printf("[core/backup] node_register sent (nodes=%d)\n", ct.node_count);
}

// CT 변경 후 공통 처리 (topology + peer sync)
static void on_ct_changed(struct mosquitto* mosq, CoreContext* ctx) {
    publish_topology(mosq, ctx);
    if (!ctx->is_backup) {
        publish_ct_sync(mosq, ctx);
    } else {
        publish_node_register(ctx);
    }
}

// Own-broker Callbacks =====================================================

static void on_connect(struct mosquitto* mosq, void* userdata, int rc) {
    if (rc != 0) {
        fprintf(stderr, "[core] connect failed (rc=%d)\n", rc);
        return;
    }
    printf("[core] connected (%s)\n", static_cast<CoreContext*>(userdata)->is_backup ? "BACKUP" : "ACTIVE");

    auto* ctx = static_cast<CoreContext*>(userdata);

    mosquitto_subscribe(mosq, nullptr, TOPIC_DATA_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_RELAY_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_STATUS_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_ELECTION_ALL, 1);

    // Active only: receive Backup's nodes
    if (!ctx->is_backup) {
        mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_REGISTER, 1);
    }

    // 초기 CT publish
    publish_topology(mosq, ctx);
    if (!ctx->is_backup) {
        publish_ct_sync(mosq, ctx);
    }
}

static void on_disconnect(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[core] disconnected (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg) {
    auto* ctx = static_cast<CoreContext*>(userdata);

    // Node 비정상 종료 LWT (W-02): OFFLINE 마킹 → CT 브로드캐스트 → node_down 알림 (FR-06)
    if (strncmp(msg->topic, "campus/will/node/", 17) == 0) {
        const char* node_id = msg->topic + 17;
        if (ctx->ct_manager->setNodeStatus(node_id, NODE_STATUS_OFFLINE)) {
            on_ct_changed(mosq, ctx);

            char alert_topic[128];
            snprintf(alert_topic, sizeof(alert_topic), "campus/alert/node_down/%s", node_id);
            ConnectionTable ct = ctx->ct_manager->snapshot();
            std::string json = connection_table_to_json(ct);
            mosquitto_publish(mosq, nullptr, alert_topic,
                (int)json.size(), json.c_str(), 1, false);

            printf("[core] node offline: %s  (ct.version=%d)\n", node_id, ct.version);
        }
        return;
    }

    // Edge 등록 (M-03): CT에 추가 후 브로드캐스트
    if (strncmp(msg->topic, "campus/monitor/status/", 22) == 0) {
        MqttMessage reg = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, reg)) return;

        char node_ip[IP_LEN] = {};
        int  node_port = 0;
        if (!parse_ip_port(reg.payload.description, node_ip, sizeof(node_ip), &node_port)) {
            fprintf(stderr, "[core] bad status description: '%s'\n", reg.payload.description);
            return;
        }

        NodeEntry node = {};
        strncpy(node.id, reg.source.id, UUID_LEN - 1);
        node.role = NODE_ROLE_NODE;
        strncpy(node.ip, node_ip, IP_LEN - 1);
        node.port = (uint16_t)node_port;
        node.status = NODE_STATUS_ONLINE;
        node.hop_to_core = 1;

        ctx->ct_manager->addNode(node);
        on_ct_changed(mosq, ctx);
        printf("[core] edge registered: %s  %s:%d\n", node.id, node_ip, node_port);
        return;
    }

    // Active only: Backup의 노드 수신 → merge → 재브로드캐스트
    if (!ctx->is_backup && strcmp(msg->topic, TOPIC_NODE_REGISTER) == 0) {
        ConnectionTable remote;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, remote)) return;

        if (merge_connection_tables(*ctx->ct_manager, remote)) {
            publish_topology(mosq, ctx);
            publish_ct_sync(mosq, ctx);
            printf("[core/active] merged backup nodes, ct.version=%d\n",
                ctx->ct_manager->snapshot().version);
        }
        return;
    }

    // 이벤트 데이터 / Relay (FR-02, FR-03): msg_id 중복 필터 후 republish
    if (strncmp(msg->topic, "campus/data/", 12) == 0 ||
        strncmp(msg->topic, "campus/relay/", 13) == 0) {
        MqttMessage evt = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, evt)) return;

        std::string msg_id(evt.msg_id);
        if (ctx->seen_msg_ids.count(msg_id)) return;

        if (ctx->seen_msg_ids.size() > 10000) ctx->seen_msg_ids.clear();
        ctx->seen_msg_ids.insert(msg_id);

        mosquitto_publish(mosq, nullptr, msg->topic,
            msg->payloadlen, msg->payload, 1, false);
        printf("[core] event forwarded: %s  (msg_id=%.8s)\n", msg->topic, evt.msg_id);
        return;
    }
}

// Peer Callbacks (Backup → Active's broker) ============================

static void on_connect_peer(struct mosquitto* mosq, void* userdata, int rc) {
    if (rc != 0) {
        fprintf(stderr, "[core/backup] peer connect failed (rc=%d)\n", rc);
        return;
    }
    printf("[core/backup] connected to active broker\n");

    // Active의 CT + LWT 구독
    mosquitto_subscribe(mosq, nullptr, TOPIC_CT_SYNC, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);

    // Backup 자신의 노드 정보 전송 → Active가 merge
    auto* ctx = static_cast<CoreContext*>(userdata);
    publish_node_register(ctx);
}

static void on_disconnect_peer(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[core/backup] disconnected from active broker (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_peer(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg) {
    auto* ctx = static_cast<CoreContext*>(userdata);

    // Active의 CT 수신 → merge → Backup의 own broker에 TOPOLOGY publish
    if (strcmp(msg->topic, TOPIC_CT_SYNC) == 0) {
        ConnectionTable remote;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, remote)) return;

        // Active의 active_core_id 반영
        if (remote.active_core_id[0] != '\0') {
            ctx->ct_manager->setActiveCoreId(remote.active_core_id);
        }

        bool changed = merge_connection_tables(*ctx->ct_manager, remote);
        if (changed) {
            // Backup's own broker에 merged CT 배포
            publish_topology(ctx->mosq_self, ctx);
            // Active에 Backup 노드 재전송 (변경이 있을 때만)
            publish_node_register(ctx);
            printf("[core/backup] merged active CT, ct.version=%d\n",
                ctx->ct_manager->snapshot().version);
        }
        (void)mosq;
        return;
    }

    // Active Core LWT 수신 (W-01): campus/alert/core_switch 발행 → Edge 재연결 유도
    if (strncmp(msg->topic, "campus/will/core/", 17) == 0) {
        printf("[core/backup] active core down: %s — promoting self\n", msg->topic + 17);

        // campus/alert/core_switch 를 Active 브로커에 발행
        // (Edges는 Active 브로커에 연결되어 있으므로 거기서 수신)
        MqttMessage sw = {};
        uuid_generate(sw.msg_id);
        sw.type = MSG_TYPE_STATUS;
        sw.source.role = NODE_ROLE_CORE;
        strncpy(sw.source.id, ctx->core_id, UUID_LEN - 1);
        snprintf(sw.payload.description, DESCRIPTION_LEN,
            "%s:%d", ctx->core_ip, ctx->core_port);

        std::string sw_json = mqtt_message_to_json(sw);
        mosquitto_publish(mosq, nullptr, "campus/alert/core_switch",
            (int)sw_json.size(), sw_json.c_str(), 1, false);

        printf("[core/backup] core_switch sent: %s:%d\n", ctx->core_ip, ctx->core_port);
        return;
    }
}

// main =====================================================

int main(int argc, char* argv[]) {
    // Active: <broker_host> <broker_port>
    // Backup: <broker_host> <broker_port> <active_core_ip> <active_core_port>
    if (argc < 3) {
        fprintf(stderr, "usage: %s <broker_host> <broker_port>"
            " [active_core_ip active_core_port]\n", argv[0]);
        return 1;
    }
    const char* broker_host = argv[1];
    int         broker_port = atoi(argv[2]);
    bool        is_backup   = (argc >= 5);
    const char* active_core_ip   = is_backup ? argv[3] : "";
    int         active_core_port = is_backup ? atoi(argv[4]) : 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. CoreContext 초기화
    CoreContext ctx = {};
    uuid_generate(ctx.core_id);
    strncpy(ctx.core_ip, broker_host, IP_LEN - 1);
    ctx.core_port   = broker_port;
    ctx.is_backup   = is_backup;
    if (is_backup) {
        strncpy(ctx.active_core_ip, active_core_ip, IP_LEN - 1);
        ctx.active_core_port = active_core_port;
    }

    // 2. mosquitto 라이브러리 초기화
    mosquitto_lib_init();

    // 3. ConnectionTable 초기화 및 self(Core) 등록
    ConnectionTableManager ct_manager;
    if (is_backup) {
        ct_manager.init("", ctx.core_id);   // active_core_id 미확정, self = backup
    } else {
        ct_manager.init(ctx.core_id, "");   // self = active
    }
    ctx.ct_manager = &ct_manager;
    {
        NodeEntry self = {};
        strncpy(self.id, ctx.core_id, UUID_LEN - 1);
        self.role = NODE_ROLE_CORE;
        strncpy(self.ip, broker_host, IP_LEN - 1);
        self.port = (uint16_t)broker_port;
        self.status = NODE_STATUS_ONLINE;
        self.hop_to_core = is_backup ? 1 : 0;
        ct_manager.addNode(self);
    }

    // 4. mosq_self 생성
    struct mosquitto* mosq = mosquitto_new(ctx.core_id, true, &ctx);
    if (!mosq) {
        fprintf(stderr, "[core] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    ctx.mosq_self = mosq;

    // 5. LWT 설정 (W-01): 자신의 종료 알림 (payload에 backup 정보 불포함)
    //    Failover 신호는 Backup Core가 campus/alert/core_switch 로 전달
    {
        MqttMessage lwt = {};
        uuid_generate(lwt.msg_id);
        lwt.type = MSG_TYPE_LWT_CORE;
        lwt.source.role = NODE_ROLE_CORE;
        strncpy(lwt.source.id, ctx.core_id, UUID_LEN - 1);

        char lwt_topic[128];
        snprintf(lwt_topic, sizeof(lwt_topic), "%s%s", TOPIC_LWT_CORE_PREFIX, ctx.core_id);

        std::string lwt_json = mqtt_message_to_json(lwt);
        mosquitto_will_set(mosq, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // 6. 콜백 등록 + 연결
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 2, 30, false);

    int rc = mosquitto_connect(mosq, broker_host, broker_port, /*keepalive=*/60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[core] mosquitto_connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_loop_start(mosq);

    printf("[core] %s (%s) running on %s:%d\n",
        ctx.core_id,
        is_backup ? "BACKUP" : "ACTIVE",
        broker_host, broker_port);

    // 7. mosq_peer 생성 (Backup only)
    struct mosquitto* mosq_peer = nullptr;
    if (is_backup) {
        char peer_id[UUID_LEN + 8];
        snprintf(peer_id, sizeof(peer_id), "%s-peer", ctx.core_id);

        mosq_peer = mosquitto_new(peer_id, true, &ctx);
        if (!mosq_peer) {
            fprintf(stderr, "[core/backup] mosquitto_new (peer) failed\n");
        } else {
            ctx.mosq_peer = mosq_peer;
            mosquitto_connect_callback_set(mosq_peer, on_connect_peer);
            mosquitto_message_callback_set(mosq_peer, on_message_peer);
            mosquitto_disconnect_callback_set(mosq_peer, on_disconnect_peer);
            mosquitto_reconnect_delay_set(mosq_peer, 2, 30, false);

            if (mosquitto_connect(mosq_peer, active_core_ip, active_core_port, 60)
                    == MOSQ_ERR_SUCCESS) {
                mosquitto_loop_start(mosq_peer);
                printf("[core/backup] peer connected to active %s:%d\n",
                    active_core_ip, active_core_port);
            } else {
                fprintf(stderr, "[core/backup] peer connect failed — retry scheduled\n");
                mosquitto_loop_start(mosq_peer);  // auto-reconnect loop 시작
            }
        }
    }

    while (g_running) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, nullptr);
    }

    // 8. 정상 종료
    printf("[core] shutting down\n");
    if (mosq_peer) {
        mosquitto_loop_stop(mosq_peer, true);
        mosquitto_destroy(mosq_peer);
    }
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
