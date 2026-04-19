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
    char                            backup_core_id[UUID_LEN]; // Active only: advertised backup ID
    char                            backup_core_ip[IP_LEN];   // Active only: advertised backup IP
    int                             backup_core_port;         // Active only: advertised backup port
    ConnectionTableManager* ct_manager;
    std::unordered_set<std::string> seen_msg_ids;
    struct mosquitto* mosq_self;  // own broker connection
    struct mosquitto* mosq_peer;  // Backup → Active's broker

    // Election state (FR-10)
    int  election_votes;               // 자신을 지지하는 투표 수
    char election_winner[UUID_LEN];    // 현재 라운드 최다득표 후보
    std::unordered_set<std::string> election_voters;  // 중복 집계 방지용 voter ID
};

static void handle_signal(int) { g_running = false; }

// CT 발행 헬퍼 =====================================================

static std::string build_core_lwt_json(const CoreContext& ctx) {
    MqttMessage lwt = {};
    uuid_generate(lwt.msg_id);
    lwt.type = MSG_TYPE_LWT_CORE;
    lwt.source.role = NODE_ROLE_CORE;
    std::strncpy(lwt.source.id, ctx.core_id, UUID_LEN - 1);
    lwt.source.id[UUID_LEN - 1] = '\0';
    lwt.delivery = { 1, false, false };

    if (!ctx.is_backup &&
        ctx.backup_core_id[0] != '\0' &&
        ctx.backup_core_ip[0] != '\0' &&
        ctx.backup_core_port > 0) {
        lwt.target.role = NODE_ROLE_CORE;
        std::strncpy(lwt.target.id, ctx.backup_core_id, UUID_LEN - 1);
        lwt.target.id[UUID_LEN - 1] = '\0';

        std::strncpy(lwt.route.original_node, ctx.core_id, UUID_LEN - 1);
        lwt.route.original_node[UUID_LEN - 1] = '\0';
        std::strncpy(lwt.route.prev_hop, ctx.core_id, UUID_LEN - 1);
        lwt.route.prev_hop[UUID_LEN - 1] = '\0';
        std::strncpy(lwt.route.next_hop, ctx.backup_core_id, UUID_LEN - 1);
        lwt.route.next_hop[UUID_LEN - 1] = '\0';
        lwt.route.ttl = 1;

        std::snprintf(lwt.payload.description, DESCRIPTION_LEN,
            "%s:%d", ctx.backup_core_ip, ctx.backup_core_port);
    }

    return mqtt_message_to_json(lwt);
}

static void publish_core_down_notice(struct mosquitto* mosq, const CoreContext& ctx) {
    if (!mosq || ctx.core_id[0] == '\0') {
        return;
    }

    char topic[128];
    std::snprintf(topic, sizeof(topic), "%s%s", TOPIC_LWT_CORE_PREFIX, ctx.core_id);
    std::string lwt_json = build_core_lwt_json(ctx);
    mosquitto_publish(mosq, nullptr, topic,
        (int)lwt_json.size(), lwt_json.c_str(), 1, false);
}

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

// 브라우저 그래프가 노드 관계를 바로 표현할 수 있도록 기본 링크를 유지한다.
static bool ensure_link(ConnectionTableManager* ct_manager,
                        const char* from_id,
                        const char* to_id,
                        float rtt_ms = 0.0f) {
    if (!from_id || !to_id || from_id[0] == '\0' || to_id[0] == '\0') {
        return false;
    }
    if (ct_manager->findLink(from_id, to_id).has_value()) {
        return false;
    }

    LinkEntry link = {};
    std::strncpy(link.from_id, from_id, UUID_LEN - 1);
    std::strncpy(link.to_id, to_id, UUID_LEN - 1);
    link.rtt_ms = rtt_ms;
    return ct_manager->addLink(link);
}

static void publish_active_view(struct mosquitto* mosq, CoreContext* ctx) {
    if (!mosq) {
        return;
    }
    publish_topology(mosq, ctx);
    publish_ct_sync(mosq, ctx);
}

static void stop_peer_channel_after_promotion(CoreContext* ctx) {
    if (!ctx || !ctx->mosq_peer) {
        return;
    }

    // 주의: 콜백 컨텍스트 안에서 호출되므로 loop_stop 은 부르지 않는다.
    // (자기 자신의 loop 스레드를 stop 하면 deadlock 위험.)
    // 실제 정리는 main() 종료부에서 loop_stop(..., true) + destroy 로 수행한다.
    mosquitto_disconnect(ctx->mosq_peer);
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
    }
    else {
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
    mosquitto_subscribe(mosq, nullptr, TOPIC_STATUS_RELAY_ALL, 1);  // peer 경유 edge 등록
    mosquitto_subscribe(mosq, nullptr, TOPIC_RTT_ALL, 1);           // edge간 RTT 보고

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

    // Backup Core 종료 감지: ACTIVE는 backup 상태를 CT와 client view에서 제거한다.
    if (strncmp(msg->topic, "campus/will/core/", 17) == 0) {
        const char* core_id = msg->topic + 17;
        if (std::strcmp(core_id, ctx->core_id) == 0) {
            return;
        }

        if (!ctx->is_backup) {
            ConnectionTable snapshot = ctx->ct_manager->snapshot();
            bool changed = false;

            if (snapshot.backup_core_id[0] != '\0' &&
                std::strncmp(snapshot.backup_core_id, core_id, UUID_LEN) == 0) {
                ctx->ct_manager->setBackupCoreId("");
                changed = true;
            }

            auto failed_core = ctx->ct_manager->findNode(core_id);
            if (failed_core.has_value() && failed_core->status != NODE_STATUS_OFFLINE) {
                changed = ctx->ct_manager->setNodeStatus(core_id, NODE_STATUS_OFFLINE) || changed;
            }

            if (changed) {
                on_ct_changed(mosq, ctx);
                printf("[core] peer core offline: %s  (ct.version=%d)\n",
                    core_id, ctx->ct_manager->snapshot().version);
            }
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

        bool changed = mark_duplicate_endpoint_nodes_offline(
            *ctx->ct_manager, node.id, node.ip, node.port);
        auto existing = ctx->ct_manager->findNode(reg.source.id);
        if (existing && existing->status == NODE_STATUS_OFFLINE) {
            // Node 복구: OFFLINE → ONLINE (FR-13, A-02)
            changed = ctx->ct_manager->updateNode(node) || changed;
            changed = ensure_link(ctx->ct_manager, ctx->core_id, reg.source.id) || changed;
            if (changed) {
                on_ct_changed(mosq, ctx);
            }

            char alert_topic[128];
            snprintf(alert_topic, sizeof(alert_topic), "campus/alert/node_up/%s", reg.source.id);
            ConnectionTable ct = ctx->ct_manager->snapshot();
            std::string ct_json = connection_table_to_json(ct);
            mosquitto_publish(mosq, nullptr, alert_topic,
                (int)ct_json.size(), ct_json.c_str(), 1, false);

            printf("[core] node recovered: %s  (ct.version=%d)\n", reg.source.id, ct.version);
        }
        else if (!existing) {
            changed = ctx->ct_manager->addNode(node) || changed;
            changed = ensure_link(ctx->ct_manager, ctx->core_id, reg.source.id) || changed;
            if (changed) {
                on_ct_changed(mosq, ctx);
            }
            printf("[core] edge registered: %s  %s:%d\n", node.id, node_ip, node_port);
        }
        else {
            if (!same_node_entry(*existing, node)) {
                changed = ctx->ct_manager->updateNode(node) || changed;
            }
            changed = ensure_link(ctx->ct_manager, ctx->core_id, reg.source.id) || changed;
            if (changed) {
                on_ct_changed(mosq, ctx);
            }
        }
        // else: already ONLINE, ignore duplicate
        return;
    }

    // Peer 경유 Edge 등록 (campus/monitor/status_relay/<forwarder_id>/<node_id>)
    // forwarder→node 링크 생성 (Core→node 직결 링크 없음), hop_to_core=2
    if (strncmp(msg->topic, TOPIC_STATUS_RELAY_PREFIX, strlen(TOPIC_STATUS_RELAY_PREFIX)) == 0) {
        MqttMessage reg = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, reg)) return;

        // 토픽에서 forwarder_id / node_id 파싱
        const char* ids_start = msg->topic + strlen(TOPIC_STATUS_RELAY_PREFIX);
        const char* slash = strchr(ids_start, '/');
        if (!slash || (slash - ids_start) >= (int)UUID_LEN) return;

        char forwarder_id[UUID_LEN] = {};
        strncpy(forwarder_id, ids_start, (size_t)(slash - ids_start));
        const char* relay_node_id = slash + 1;

        char node_ip[IP_LEN] = {};
        int  node_port = 0;
        if (!parse_ip_port(reg.payload.description, node_ip, sizeof(node_ip), &node_port)) {
            fprintf(stderr, "[core] bad relay status description: '%s'\n", reg.payload.description);
            return;
        }

        NodeEntry node = {};
        strncpy(node.id, relay_node_id, UUID_LEN - 1);
        node.role = NODE_ROLE_NODE;
        strncpy(node.ip, node_ip, IP_LEN - 1);
        node.port = (uint16_t)node_port;
        node.status = NODE_STATUS_ONLINE;
        node.hop_to_core = 2;

        bool changed = false;
        auto existing = ctx->ct_manager->findNode(relay_node_id);
        if (!existing) {
            changed = ctx->ct_manager->addNode(node) || changed;
        } else if (existing->status == NODE_STATUS_OFFLINE) {
            changed = ctx->ct_manager->updateNode(node) || changed;
        }
        // forwarder→node 링크 (직결이 아닌 peer 경로)
        changed = ensure_link(ctx->ct_manager, forwarder_id, relay_node_id) || changed;

        if (changed) {
            on_ct_changed(mosq, ctx);
            printf("[core] peer-relay edge registered: %s via %s  %s:%d\n",
                relay_node_id, forwarder_id, node_ip, node_port);
        }
        return;
    }

    // Edge간 RTT 보고 (campus/monitor/rtt/<from_id>/<to_id>) → CT 링크 RTT 업데이트
    if (strncmp(msg->topic, TOPIC_RTT_PREFIX, strlen(TOPIC_RTT_PREFIX)) == 0) {
        const char* ids_start = msg->topic + strlen(TOPIC_RTT_PREFIX);
        const char* slash = strchr(ids_start, '/');
        if (!slash || (slash - ids_start) >= (int)UUID_LEN) return;

        char from_id[UUID_LEN] = {};
        strncpy(from_id, ids_start, (size_t)(slash - ids_start));
        const char* to_id = slash + 1;

        std::string payload_str(static_cast<char*>(msg->payload), msg->payloadlen);
        float rtt_ms = (float)atof(payload_str.c_str());
        if (rtt_ms <= 0.0f) return;

        LinkEntry link = {};
        strncpy(link.from_id, from_id, UUID_LEN - 1);
        strncpy(link.to_id, to_id, UUID_LEN - 1);
        link.rtt_ms = rtt_ms;
        if (ctx->ct_manager->addLink(link)) {
            on_ct_changed(mosq, ctx);
            printf("[core] RTT link updated: %s→%s  %.2fms\n", from_id, to_id, rtt_ms);
        }
        return;
    }

    // Active only: Backup의 노드 수신 → merge → 재브로드캐스트
    if (!ctx->is_backup && strcmp(msg->topic, TOPIC_NODE_REGISTER) == 0) {
        ConnectionTable remote;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, remote)) return;

        // node_register 는 backup bootstrap/재등록 채널이다.
        // CT_SYNC 와 달리 같은 version 이어도 backup_core_id 와 신규 노드를 merge 해야 한다.
        bool changed = merge_backup_registration(*ctx->ct_manager, ctx->core_id, remote);

        if (changed) {
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
        // R-02: application-level ACK → Edge의 pending_msgs에서 해당 메시지 제거
        {
            char ack_topic[128];
            std::snprintf(ack_topic, sizeof(ack_topic), "%s%s", TOPIC_RELAY_ACK_PREFIX, evt.msg_id);
            mosquitto_publish(mosq, nullptr, ack_topic, 0, nullptr, 0, false);
        }
        return;
    }

    // Election 요청 수신 (C-03): 요청자를 후보로 투표 발행 (FR-10)
    if (strcmp(msg->topic, TOPIC_ELECTION_REQUEST) == 0) {
        MqttMessage req = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, req)) return;

        // 자신이 보낸 요청은 무시
        if (strcmp(req.source.id, ctx->core_id) == 0) return;

        // 요청자를 후보로 투표 (요청자가 선출을 원하는 것으로 간주)
        MqttMessage vote = {};
        uuid_generate(vote.msg_id);
        vote.type = MSG_TYPE_ELECTION_RESULT;
        vote.source.role = NODE_ROLE_CORE;
        strncpy(vote.source.id, ctx->core_id, UUID_LEN - 1);
        strncpy(vote.payload.description, req.source.id, DESCRIPTION_LEN - 1);

        std::string vote_json = mqtt_message_to_json(vote);
        mosquitto_publish(mosq, nullptr, TOPIC_ELECTION_RESULT,
            (int)vote_json.size(), vote_json.c_str(), 1, false);

        printf("[core] election vote sent: candidate=%s\n", req.source.id);
        return;
    }

    // Election 결과 수신 (C-04): 투표 집계 → 과반 시 ACTIVE 선언 (FR-10)
    if (strcmp(msg->topic, TOPIC_ELECTION_RESULT) == 0) {
        MqttMessage res = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, res)) return;

        // 자신이 보낸 투표는 무시
        if (strcmp(res.source.id, ctx->core_id) == 0) return;

        // 중복 voter 무시
        if (ctx->election_voters.count(res.source.id)) return;

        if (strcmp(res.payload.description, ctx->core_id) == 0) {
            ctx->election_voters.insert(res.source.id);
            ctx->election_votes++;
            printf("[core] election vote received for self (%d votes)\n", ctx->election_votes);

            // 과반 달성 (2-core 환경에서는 1표면 충분)
            if (ctx->election_votes >= 1) {
                ConnectionTable snapshot = ctx->ct_manager->snapshot();
                promote_core_after_failover(*ctx->ct_manager, ctx->core_id, snapshot.active_core_id);
                // is_backup 플래그를 먼저 내려야 on_ct_changed 가 active 경로(publish_ct_sync)로 분기한다.
                ctx->is_backup = false;
                mosquitto_subscribe(ctx->mosq_self, nullptr, TOPIC_NODE_REGISTER, 1);
                on_ct_changed(mosq, ctx);

                // core_switch 발행 → Edge들이 수신하여 새 Active Core로 전환
                MqttMessage sw = {};
                uuid_generate(sw.msg_id);
                sw.type = MSG_TYPE_STATUS;
                sw.source.role = NODE_ROLE_CORE;
                strncpy(sw.source.id, ctx->core_id, UUID_LEN - 1);
                snprintf(sw.payload.description, DESCRIPTION_LEN,
                    "%s:%d", ctx->core_ip, ctx->core_port);
                std::string sw_json = mqtt_message_to_json(sw);
                int mid = 0;
                int rc = mosquitto_publish(mosq, &mid, "campus/alert/core_switch",
                    (int)sw_json.size(), sw_json.c_str(), 1, false);
                if (rc != MOSQ_ERR_SUCCESS) {
                    fprintf(stderr, "[core] core_switch publish failed: %s\n",
                        mosquitto_strerror(rc));
                }

                ctx->election_votes = 0;
                ctx->election_voters.clear();
                printf("[core] elected as ACTIVE (election complete), core_switch sent (mid=%d)\n", mid);
            }
        }
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

    auto* ctx = static_cast<CoreContext*>(userdata);
    if (!ctx->is_backup) {
        mosquitto_disconnect(mosq);
        return;
    }

    // Active의 CT + LWT + Election 구독
    mosquitto_subscribe(mosq, nullptr, TOPIC_CT_SYNC, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_ELECTION_ALL, 1);

    // Backup 자신의 노드 정보 전송 → Active가 merge
    publish_node_register(ctx);
}

static void on_disconnect_peer(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[core/backup] disconnected from active broker (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}

static void on_message_peer(struct mosquitto* mosq, void* userdata,
    const struct mosquitto_message* msg) {
    auto* ctx = static_cast<CoreContext*>(userdata);

    if (!ctx->is_backup) {
        return;
    }

    // Active의 CT 수신 → merge → Backup의 own broker에 TOPOLOGY publish
    if (strcmp(msg->topic, TOPIC_CT_SYNC) == 0) {
        ConnectionTable remote;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, remote)) return;

        // 구버전 CT 무시 (FR-01)
        int local_ver = ctx->ct_manager->snapshot().version;
        if (remote.version <= local_ver) {
            printf("[core/backup] skip stale CT (remote=%d <= local=%d)\n",
                remote.version, local_ver);
            return;
        }

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
        const char* failed_core_id = msg->topic + 17;
        if (!should_promote_backup_on_core_will(*ctx->ct_manager, ctx->core_id, failed_core_id)) {
            printf("[core/backup] ignoring non-active core down: %s\n", failed_core_id);
            return;
        }

        printf("[core/backup] active core down: %s — promoting self\n", failed_core_id);

        promote_core_after_failover(*ctx->ct_manager, ctx->core_id, failed_core_id);
        ctx->is_backup = false;
        mosquitto_subscribe(ctx->mosq_self, nullptr, TOPIC_NODE_REGISTER, 1);

        publish_active_view(ctx->mosq_self, ctx);
        if (mosq != ctx->mosq_self) {
            publish_active_view(mosq, ctx);
        }

        // campus/alert/core_switch 를 자신(= 새 Active) 브로커에 발행한다.
        // 주의: 기존 Active 브로커(= mosq_peer 가 붙어있던 곳)는 방금 죽었으므로
        // 그쪽으로 publish 하면 메시지는 송신 큐에만 쌓이고 네트워크로는 나가지 않는다.
        // Edge 들은 campus/alert/core_switch 수신 후 payload.description 에 담긴
        // 새 Active(= 자신) 로 재연결하므로, 반드시 자기 자신의 브로커에 발행해야 한다.
        MqttMessage sw = {};
        uuid_generate(sw.msg_id);
        sw.type = MSG_TYPE_STATUS;
        sw.source.role = NODE_ROLE_CORE;
        strncpy(sw.source.id, ctx->core_id, UUID_LEN - 1);
        snprintf(sw.payload.description, DESCRIPTION_LEN,
            "%s:%d", ctx->core_ip, ctx->core_port);

        std::string sw_json = mqtt_message_to_json(sw);
        int mid = 0;
        int rc = mosquitto_publish(ctx->mosq_self, &mid, "campus/alert/core_switch",
            (int)sw_json.size(), sw_json.c_str(), 1, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[core/backup] core_switch publish failed: %s\n",
                mosquitto_strerror(rc));
        }

        stop_peer_channel_after_promotion(ctx);
        printf("[core/backup] core_switch sent on self broker (mid=%d): %s:%d\n",
            mid, ctx->core_ip, ctx->core_port);
        return;
    }

    // peer 브로커에서 Election 요청 수신 (C-03): 요청자를 후보로 투표를 peer 브로커에 발행
    if (strcmp(msg->topic, TOPIC_ELECTION_REQUEST) == 0) {
        MqttMessage req = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, req)) return;

        if (strcmp(req.source.id, ctx->core_id) == 0) return;

        MqttMessage vote = {};
        uuid_generate(vote.msg_id);
        vote.type = MSG_TYPE_ELECTION_RESULT;
        vote.source.role = NODE_ROLE_CORE;
        strncpy(vote.source.id, ctx->core_id, UUID_LEN - 1);
        strncpy(vote.payload.description, req.source.id, DESCRIPTION_LEN - 1);

        std::string vote_json = mqtt_message_to_json(vote);
        // peer 브로커(Active 브로커)에 발행 → Active Core의 on_message()가 수신
        mosquitto_publish(mosq, nullptr, TOPIC_ELECTION_RESULT,
            (int)vote_json.size(), vote_json.c_str(), 1, false);

        printf("[core/backup] election vote sent via peer: candidate=%s\n", req.source.id);
        return;
    }

    // peer 브로커에서 Election 결과 수신 (C-04): 집계 → 과반 시 자신 브로커에 core_switch 발행
    if (strcmp(msg->topic, TOPIC_ELECTION_RESULT) == 0) {
        MqttMessage res = {};
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!mqtt_message_from_json(json, res)) return;

        if (strcmp(res.source.id, ctx->core_id) == 0) return;
        if (ctx->election_voters.count(res.source.id)) return;

        if (strcmp(res.payload.description, ctx->core_id) == 0) {
            ctx->election_voters.insert(res.source.id);
            ctx->election_votes++;
            printf("[core/backup] election vote received via peer for self (%d votes)\n",
                ctx->election_votes);

            if (ctx->election_votes >= 1) {
                ConnectionTable snapshot = ctx->ct_manager->snapshot();
                promote_core_after_failover(*ctx->ct_manager, ctx->core_id, snapshot.active_core_id);
                ctx->is_backup = false;
                mosquitto_subscribe(ctx->mosq_self, nullptr, TOPIC_NODE_REGISTER, 1);
                // 자신 브로커 topology 갱신
                publish_active_view(ctx->mosq_self, ctx);
                if (mosq != ctx->mosq_self) {
                    publish_active_view(mosq, ctx);
                }
                // core_switch 는 자기 자신의 브로커에 발행해야 한다.
                // Edge 들이 붙게 될 새 Active 브로커가 바로 여기이기 때문이다.
                MqttMessage sw = {};
                uuid_generate(sw.msg_id);
                sw.type = MSG_TYPE_STATUS;
                sw.source.role = NODE_ROLE_CORE;
                strncpy(sw.source.id, ctx->core_id, UUID_LEN - 1);
                snprintf(sw.payload.description, DESCRIPTION_LEN,
                    "%s:%d", ctx->core_ip, ctx->core_port);
                std::string sw_json = mqtt_message_to_json(sw);
                int mid = 0;
                int rc = mosquitto_publish(ctx->mosq_self, &mid, "campus/alert/core_switch",
                    (int)sw_json.size(), sw_json.c_str(), 1, false);
                if (rc != MOSQ_ERR_SUCCESS) {
                    fprintf(stderr, "[core/backup] core_switch publish failed: %s\n",
                        mosquitto_strerror(rc));
                }

                ctx->election_votes = 0;
                ctx->election_voters.clear();
                stop_peer_channel_after_promotion(ctx);
                printf("[core/backup] elected as ACTIVE via peer election, core_switch sent on self broker (mid=%d)\n",
                    mid);
            }
        }
        return;
    }
}

// main =====================================================

int main(int argc, char* argv[]) {
    // Active: <broker_host> <broker_port>
    // Active(with backup hint): <broker_host> <broker_port> <backup_core_id> <backup_core_ip> <backup_core_port>
    // Backup: <broker_host> <broker_port> <active_core_ip> <active_core_port>
    if (argc != 3 && argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s <broker_host> <broker_port>"
            " [active_core_ip active_core_port]\n"
            "   or: %s <broker_host> <broker_port> <backup_core_id> <backup_core_ip> <backup_core_port>\n",
            argv[0], argv[0]);
        return 1;
    }
    const char* broker_host = argv[1];
    int         broker_port = atoi(argv[2]);
    bool        is_backup = (argc == 5);
    const char* active_core_ip = is_backup ? argv[3] : "";
    int         active_core_port = is_backup ? atoi(argv[4]) : 0;
    const char* backup_core_id = (argc == 6) ? argv[3] : "";
    const char* backup_core_ip = (argc == 6) ? argv[4] : "";
    int         backup_core_port = (argc == 6) ? atoi(argv[5]) : 0;

    setvbuf(stdout, nullptr, _IOLBF, 0);  // 테스트 스크립트가 로그를 실시간 grep할 수 있도록 line-buffered 설정
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 1. CoreContext 초기화
    CoreContext ctx = {};
    // core_id 파일 영속화: 재시작 후에도 동일 UUID 유지 (CT continuity 보장)
    {
        char id_path[256];
        std::snprintf(id_path, sizeof(id_path), "/tmp/core_id_%d.txt", broker_port);
        FILE* id_file = fopen(id_path, "r");
        if (id_file) {
            size_t n = fread(ctx.core_id, 1, UUID_LEN - 1, id_file);
            ctx.core_id[n] = '\0';
            fclose(id_file);
            printf("[core] restored core_id from file: %.8s...\n", ctx.core_id);
        } else {
            uuid_generate(ctx.core_id);
            id_file = fopen(id_path, "w");
            if (id_file) {
                fwrite(ctx.core_id, 1, strlen(ctx.core_id), id_file);
                fclose(id_file);
            }
            printf("[core] generated new core_id: %.8s...\n", ctx.core_id);
        }
    }
    strncpy(ctx.core_ip, broker_host, IP_LEN - 1);
    ctx.core_port = broker_port;
    ctx.is_backup = is_backup;
    if (is_backup) {
        strncpy(ctx.active_core_ip, active_core_ip, IP_LEN - 1);
        ctx.active_core_port = active_core_port;
    }
    else if (argc == 6) {
        strncpy(ctx.backup_core_id, backup_core_id, UUID_LEN - 1);
        strncpy(ctx.backup_core_ip, backup_core_ip, IP_LEN - 1);
        ctx.backup_core_port = backup_core_port;
    }

    // 2. mosquitto 라이브러리 초기화
    mosquitto_lib_init();

    // 3. ConnectionTable 초기화 및 self(Core) 등록
    ConnectionTableManager ct_manager;
    if (is_backup) {
        ct_manager.init("", ctx.core_id);   // active_core_id 미확정, self = backup
    }
    else {
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

    // 5. LWT 설정 (W-01): Active는 backup endpoint 힌트를 포함할 수 있다.
    //    Backup의 실제 승격 알림은 campus/alert/core_switch 로 별도 전달된다.
    {
        char lwt_topic[128];
        snprintf(lwt_topic, sizeof(lwt_topic), "%s%s", TOPIC_LWT_CORE_PREFIX, ctx.core_id);

        std::string lwt_json = build_core_lwt_json(ctx);
        mosquitto_will_set(mosq, lwt_topic,
            (int)lwt_json.size(), lwt_json.c_str(), 1, false);
    }

    // 6. 콜백 등록 + 연결
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 2, 30, false);

    int rc = mosquitto_connect(mosq, broker_host, broker_port, /*keepalive=*/10);
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
        }
        else {
            ctx.mosq_peer = mosq_peer;
            mosquitto_connect_callback_set(mosq_peer, on_connect_peer);
            mosquitto_message_callback_set(mosq_peer, on_message_peer);
            mosquitto_disconnect_callback_set(mosq_peer, on_disconnect_peer);
            mosquitto_reconnect_delay_set(mosq_peer, 2, 30, false);

            {
                char lwt_topic[128];
                snprintf(lwt_topic, sizeof(lwt_topic), "%s%s", TOPIC_LWT_CORE_PREFIX, ctx.core_id);
                std::string lwt_json = build_core_lwt_json(ctx);
                mosquitto_will_set(mosq_peer, lwt_topic,
                    (int)lwt_json.size(), lwt_json.c_str(), 1, false);
            }

            if (mosquitto_connect(mosq_peer, active_core_ip, active_core_port, 10)
                == MOSQ_ERR_SUCCESS) {
                mosquitto_loop_start(mosq_peer);
                printf("[core/backup] peer connected to active %s:%d\n",
                    active_core_ip, active_core_port);
            }
            else {
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
    publish_core_down_notice(mosq, ctx);
    if (mosq_peer) {
        publish_core_down_notice(mosq_peer, ctx);
    }
    {
        struct timespec flush_ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&flush_ts, nullptr);
    }
    if (mosq_peer) {
        mosquitto_loop_stop(mosq_peer, true);
        mosquitto_destroy(mosq_peer);
    }
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
