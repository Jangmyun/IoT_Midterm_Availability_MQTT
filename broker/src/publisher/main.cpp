// pub_sim — Publisher Event Simulator
//
// Core/Edge 성능·가용성 검증용 MQTT 이벤트 발행기
// 사용법: pub_sim --help
//
// 주요 기능:
//   - 랜덤 이벤트(MOTION/DOOR_FORCED/INTRUSION) MQTT 발행
//   - 초당 이벤트 수(--rate) 또는 최대 속도 버스트(--burst)
//   - 중복 메시지 주입(--dup): Core dedup 로직 검증
//   - 멀티 퍼블리셔(--multi-pub): 다수 Edge 동시 접속 시뮬레이션
//   - Edge 등록 메시지 전송(--register)
//   - Connection Table 기반 자동 Failover (Phase 7)
//     - CT 수신 → Edge 장애 시 RTT+hop_to_core 기반 대체 브로커 자동 선택
//     - Primary Edge 복구 시 자동 복귀
//     - 재연결 중 Store-and-Forward 큐
//   - 종료 시 처리량 요약 출력

#include <cstdio>
#include <cstring>
#include <csignal>
#include <chrono>
#include <string>
#include <deque>
#include <mutex>

#ifdef _WIN32
#  include <windows.h>
#  define usleep(us) Sleep((us) / 1000)
#else
#  include <unistd.h>
#endif

#include <mosquitto.h>

#include "publisher_helpers.h"
#include "mqtt_json.h"
#include "connection_table_manager.h"

// ── 시그널 처리 ───────────────────────────────────────────────────────────────

static volatile bool g_stop = false;

static void on_signal(int) { g_stop = true; }

// ── Publisher Context ─────────────────────────────────────────────────────────

struct PubContext {
    PublisherConfig* cfg;
    struct mosquitto* mosq;
    ConnectionTableManager ct_manager;
    int last_ct_version;

    // Primary edge: 최초 연결한 브로커 정보
    char primary_ip[IP_LEN];
    int  primary_port;
    char primary_edge_id[UUID_LEN];  // CT에서 매칭되면 채워짐

    // 현재 연결 중인 브로커 정보
    char current_ip[IP_LEN];
    int  current_port;
    bool connected;

    // failover 상태
    bool on_fallback;  // 현재 대체 브로커에 연결 중인지

    // CT가 비어 있어도 마지막으로 본 core endpoint로 직접 붙을 수 있게 유지
    char cached_core_ip[IP_LEN];
    int  cached_core_port;
    bool has_cached_core;

    // store-and-forward queue
    struct QueuedEvent {
        char topic[256];
        std::string json;
    };
    std::deque<QueuedEvent> store_queue;
    std::mutex queue_mutex;
};

// ── 사용법 출력 ───────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    fprintf(stderr,
        "사용법: %s [옵션]\n"
        "\n"
        "연결:\n"
        "  --host   <addr>    브로커 주소         (기본: localhost)\n"
        "  --port   <port>    브로커 포트         (기본: 1883)\n"
        "  --id     <uuid>    고정 publisher UUID (기본: auto)\n"
        "\n"
        "이벤트:\n"
        "  --count  <n>       전송 횟수 (0=무제한, 기본: 10)\n"
        "  --rate   <hz>      초당 이벤트 수      (기본: 1)\n"
        "  --events <list>    쉼표 구분: motion,door,intrusion (기본: 전체)\n"
        "  --building <id>    빌딩 ID             (기본: building-a)\n"
        "  --camera   <id>    카메라 ID           (기본: cam-01)\n"
        "  --desc     <str>   payload.description (기본: sim-pub)\n"
        "  --qos    <0|1|2>   MQTT QoS            (기본: 1)\n"
        "\n"
        "테스트 모드:\n"
        "  --burst            최대 속도로 전송 (rate 무시)\n"
        "  --dup    [n]       이벤트마다 n번 중복 재전송 (기본 n=1)\n"
        "  --register         루프 전 STATUS 등록 메시지 전송\n"
        "  --multi-pub        이벤트마다 새 UUID (다수 Edge 시뮬레이션)\n"
        "  --verbose          전송 메시지 출력\n"
        "\n"
        "예시:\n"
        "  %s --host 192.168.1.10 --count 1000 --rate 50 --events motion,intrusion\n"
        "  %s --burst --count 5000 --multi-pub\n"
        "  %s --dup 2 --count 100 --events intrusion\n",
        prog, prog, prog, prog);
}

// ── STATUS 등록 메시지 전송 ───────────────────────────────────────────────────

static void publish_registration(struct mosquitto* mosq,
                                  const char* publisher_id, int qos)
{
    MqttMessage msg = {};
    uuid_generate(msg.msg_id);
    msg.type     = MSG_TYPE_STATUS;
    msg.priority = PRIORITY_NONE;
    set_now_utc(msg.timestamp, sizeof(msg.timestamp));
    msg.source.role = NODE_ROLE_NODE;
    std::strncpy(msg.source.id, publisher_id, UUID_LEN - 1);
    msg.target.role = NODE_ROLE_CORE;
    msg.delivery.qos = qos;
    // description에 더미 ip:port 넣기 (Core 등록 경로 실행용)
    std::snprintf(msg.payload.description, DESCRIPTION_LEN - 1, "127.0.0.1:9999");

    char topic[256];
    std::snprintf(topic, sizeof(topic), "%s%s", TOPIC_STATUS_PREFIX, publisher_id);

    std::string json = mqtt_message_to_json(msg);
    mosquitto_publish(mosq, nullptr, topic, (int)json.size(),
                      json.c_str(), qos, false);
    printf("[register] topic=%s\n", topic);
}

// ── Store-and-Forward ─────────────────────────────────────────────────────────

static void queue_event(PubContext* ctx, const char* topic, const std::string& json)
{
    std::lock_guard<std::mutex> lock(ctx->queue_mutex);
    PubContext::QueuedEvent item;
    std::strncpy(item.topic, topic, sizeof(item.topic) - 1);
    item.topic[sizeof(item.topic) - 1] = '\0';
    item.json = json;
    ctx->store_queue.push_back(item);
    printf("[pub_sim] queued event (queue_size=%zu)\n", ctx->store_queue.size());
}

static void flush_store_queue(PubContext* ctx)
{
    std::lock_guard<std::mutex> lock(ctx->queue_mutex);
    while (!ctx->store_queue.empty())
    {
        auto& item = ctx->store_queue.front();
        int rc = mosquitto_publish(ctx->mosq, nullptr, item.topic,
                                    (int)item.json.size(), item.json.c_str(),
                                    ctx->cfg->qos, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            printf("[pub_sim] flush failed, keeping %zu events in queue\n",
                   ctx->store_queue.size());
            return;
        }
        printf("[pub_sim] flushed queued event: topic=%s\n", item.topic);
        ctx->store_queue.pop_front();
    }
}

// ── Failover: CT 기반 대체 브로커 재연결 ──────────────────────────────────────

static void attempt_failover(PubContext* ctx)
{
    ConnectionTable ct = ctx->ct_manager.snapshot();
    if (ct.node_count == 0) {
        if (!ctx->has_cached_core) {
            printf("[pub_sim] no CT available, cannot failover\n");
            return;
        }

        printf("[pub_sim] CT empty, reconnecting directly to cached core %s:%d\n",
               ctx->cached_core_ip, ctx->cached_core_port);
        std::strncpy(ctx->current_ip, ctx->cached_core_ip, IP_LEN - 1);
        ctx->current_port = ctx->cached_core_port;
        ctx->on_fallback = true;

        mosquitto_disconnect(ctx->mosq);
        mosquitto_connect_async(ctx->mosq, ctx->current_ip, ctx->current_port, 60);
        return;
    }

    FallbackBroker fb = select_fallback_broker(ct, ctx->primary_edge_id);
    if (!fb.found) {
        printf("[pub_sim] no fallback broker found in CT\n");
        return;
    }

    printf("[pub_sim] failover: reconnecting to %s:%d (id=%.8s...)\n",
           fb.ip, fb.port, fb.id);

    std::strncpy(ctx->current_ip, fb.ip, IP_LEN - 1);
    ctx->current_port = fb.port;
    ctx->on_fallback = true;

    mosquitto_disconnect(ctx->mosq);
    mosquitto_connect_async(ctx->mosq, ctx->current_ip, ctx->current_port, 60);
}

static void attempt_return_to_primary(PubContext* ctx)
{
    if (!ctx->on_fallback) return;

    ConnectionTable ct = ctx->ct_manager.snapshot();
    if (!should_return_to_primary(ct, ctx->primary_edge_id)) return;

    printf("[pub_sim] primary edge recovered, returning to %s:%d\n",
           ctx->primary_ip, ctx->primary_port);

    std::strncpy(ctx->current_ip, ctx->primary_ip, IP_LEN - 1);
    ctx->current_port = ctx->primary_port;
    ctx->on_fallback = false;

    mosquitto_disconnect(ctx->mosq);
    mosquitto_connect_async(ctx->mosq, ctx->current_ip, ctx->current_port, 60);
}

// ── Resolve primary edge ID from CT ───────────────────────────────────────────

static void try_resolve_primary_edge_id(PubContext* ctx, const ConnectionTable& ct)
{
    if (ctx->primary_edge_id[0] != '\0')
    {
        for (int i = 0; i < ct.node_count; i++)
        {
            const NodeEntry& n = ct.nodes[i];
            if (std::strncmp(n.id, ctx->primary_edge_id, UUID_LEN) == 0 &&
                n.role == NODE_ROLE_NODE &&
                std::strncmp(n.ip, ctx->primary_ip, IP_LEN) == 0 &&
                n.port == (uint16_t)ctx->primary_port)
            {
                return;
            }
        }
    }

    for (int i = 0; i < ct.node_count; i++)
    {
        const NodeEntry& n = ct.nodes[i];
        if (n.role == NODE_ROLE_NODE &&
            std::strncmp(n.ip, ctx->primary_ip, IP_LEN) == 0 &&
            n.port == (uint16_t)ctx->primary_port)
        {
            std::strncpy(ctx->primary_edge_id, n.id, UUID_LEN - 1);
            ctx->primary_edge_id[UUID_LEN - 1] = '\0';
            printf("[pub_sim] resolved primary edge id: %s\n", ctx->primary_edge_id);
            return;
        }
    }
}

static void remember_cached_core(PubContext* ctx, const ConnectionTable& ct)
{
    FallbackBroker core = select_preferred_core_broker(ct, ctx->primary_edge_id);
    if (!core.found) return;

    std::strncpy(ctx->cached_core_ip, core.ip, IP_LEN - 1);
    ctx->cached_core_ip[IP_LEN - 1] = '\0';
    ctx->cached_core_port = core.port;
    ctx->has_cached_core = true;
}

// ── MQTT Callbacks ────────────────────────────────────────────────────────────

static void on_connect(struct mosquitto* mosq, void* userdata, int rc)
{
    auto* ctx = static_cast<PubContext*>(userdata);

    if (rc != 0) {
        ctx->connected = false;
        fprintf(stderr, "[pub_sim] connect failed (rc=%d)\n", rc);
        return;
    }

    ctx->connected = true;
    printf("[pub_sim] connected to %s:%d%s\n",
           ctx->current_ip, ctx->current_port,
           ctx->on_fallback ? " (fallback)" : " (primary)");

    // CT 구독: 대체 브로커 정보 + primary edge 복구 감지
    mosquitto_subscribe(mosq, nullptr, TOPIC_TOPOLOGY, 1);
    // Node LWT 구독: Edge 장애 사전 감지
    mosquitto_subscribe(mosq, nullptr, "campus/will/node/#", 1);
    // Core switch 구독: Active Core 변경 감지
    mosquitto_subscribe(mosq, nullptr, "campus/alert/core_switch", 1);

    // 연결 후 큐 flush
    flush_store_queue(ctx);
}

static void on_disconnect(struct mosquitto* /*mosq*/, void* userdata, int rc)
{
    auto* ctx = static_cast<PubContext*>(userdata);
    ctx->connected = false;
    printf("[pub_sim] disconnected (rc=%d)%s\n", rc,
           rc != 0 ? " — will attempt failover" : "");

    if (rc != 0) {
        // 비정상 연결 끊김 → failover 시도
        attempt_failover(ctx);
    }
}

static void on_message(struct mosquitto* /*mosq*/, void* userdata,
                        const struct mosquitto_message* msg)
{
    auto* ctx = static_cast<PubContext*>(userdata);

    // CT 수신 (M-04): 로컬 CT 갱신
    if (std::strcmp(msg->topic, TOPIC_TOPOLOGY) == 0)
    {
        ConnectionTable ct;
        std::string json(static_cast<char*>(msg->payload), msg->payloadlen);
        if (!connection_table_from_json(json, ct)) return;

        if (ct.version <= ctx->last_ct_version) return;

        // CT 전체 스냅샷 적용
        ctx->ct_manager.replace(ct);

        ctx->last_ct_version = ct.version;
        printf("[pub_sim] CT applied (version=%d, nodes=%d)\n", ct.version, ct.node_count);

        // primary edge ID 해석 시도
        try_resolve_primary_edge_id(ctx, ct);
        remember_cached_core(ctx, ct);

        // fallback 중이면 primary edge 복구 확인
        if (ctx->on_fallback) {
            attempt_return_to_primary(ctx);
        }
        return;
    }

    // Node LWT 수신: Edge 장애 사전 감지
    if (std::strncmp(msg->topic, "campus/will/node/", 17) == 0)
    {
        const char* failed_node_id = msg->topic + 17;
        printf("[pub_sim] edge down detected: %s\n", failed_node_id);

        // CT에서 해당 노드를 OFFLINE으로 마킹
        ctx->ct_manager.setNodeStatus(failed_node_id, NODE_STATUS_OFFLINE);

        // 현재 연결 중인 primary edge가 죽은 경우
        if (ctx->primary_edge_id[0] != '\0' &&
            std::strncmp(ctx->primary_edge_id, failed_node_id, UUID_LEN) == 0 &&
            !ctx->on_fallback)
        {
            printf("[pub_sim] primary edge down, triggering failover\n");
            attempt_failover(ctx);
        }
        return;
    }

    // Core switch 수신: Active Core 변경
    if (std::strcmp(msg->topic, "campus/alert/core_switch") == 0)
    {
        printf("[pub_sim] core_switch received\n");
        return;
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // --help
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    PublisherConfig cfg;
    if (!parse_publisher_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    // 시그널 등록 (Ctrl+C로 무제한 모드 종료)
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ── PubContext 초기화 ─────────────────────────────────────────────────────
    PubContext ctx = {};
    ctx.cfg = &cfg;
    ctx.last_ct_version = 0;
    ctx.connected = false;
    ctx.on_fallback = false;
    ctx.has_cached_core = false;
    std::strncpy(ctx.primary_ip, cfg.broker_host, IP_LEN - 1);
    ctx.primary_port = cfg.broker_port;
    std::strncpy(ctx.current_ip, cfg.broker_host, IP_LEN - 1);
    ctx.current_port = cfg.broker_port;
    ctx.primary_edge_id[0] = '\0';
    ctx.ct_manager.init("", "");

    // ── mosquitto 초기화 ──────────────────────────────────────────────────────
    mosquitto_lib_init();
    setvbuf(stdout, nullptr, _IOLBF, 0);

    struct mosquitto* mosq = mosquitto_new(cfg.publisher_id, true, &ctx);
    if (!mosq) {
        fprintf(stderr, "오류: mosquitto_new 실패\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    ctx.mosq = mosq;

    // 콜백 등록
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_reconnect_delay_set(mosq, 1, 10, false);

    if (mosquitto_connect(mosq, cfg.broker_host, cfg.broker_port, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "오류: 브로커 연결 실패 (%s:%d)\n",
                cfg.broker_host, cfg.broker_port);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // threaded loop 사용 — CT 수신 및 failover가 비동기로 동작
    mosquitto_loop_start(mosq);

    printf("[pub_sim] 브로커 %s:%d 연결 완료 (id=%s)\n",
           cfg.broker_host, cfg.broker_port, cfg.publisher_id);

    // ── Edge 등록 ─────────────────────────────────────────────────────────────
    if (cfg.register_edge) {
        publish_registration(mosq, cfg.publisher_id, cfg.qos);
        usleep(100000);  // 100ms — ACK 처리 대기
    }

    // ── 이벤트 루프 ──────────────────────────────────────────────────────────
    int   event_state = 0;
    int   sent        = 0;
    long  sleep_us    = cfg.burst_mode ? 0 : rate_to_sleep_us(cfg.rate_hz);
    char  pub_id[UUID_LEN];
    std::strncpy(pub_id, cfg.publisher_id, UUID_LEN - 1);

    auto t_start = std::chrono::steady_clock::now();

    while (!g_stop && (cfg.count == 0 || sent < cfg.count)) {
        // 멀티 퍼블리셔 모드: 이벤트마다 새 UUID
        if (cfg.multi_pub) {
            uuid_generate(pub_id);
        }

        MsgType type = next_event_type(cfg.event_mask, &event_state);
        const char* type_str = msg_type_to_topic_segment(type);

        MqttMessage msg;
        if (!build_event_message(pub_id, type,
                                  cfg.building_id, cfg.camera_id,
                                  cfg.description, cfg.qos, &msg)) {
            continue;
        }

        if (ctx.primary_edge_id[0] != '\0') {
            std::strncpy(msg.route.original_node, ctx.primary_edge_id, UUID_LEN - 1);
            msg.route.original_node[UUID_LEN - 1] = '\0';
        }

        char topic[256];
        if (!build_event_topic(type_str, cfg.building_id, cfg.camera_id,
                                topic, sizeof(topic))) {
            continue;
        }

        std::string json = mqtt_message_to_json(msg);

        // 연결 중이면 즉시 전송, 아니면 큐에 저장
        if (ctx.connected) {
            int rc = mosquitto_publish(mosq, nullptr, topic, (int)json.size(),
                                       json.c_str(), cfg.qos, false);
            if (rc == MOSQ_ERR_SUCCESS) {
                sent++;
                if (cfg.verbose) {
                    printf("[pub] #%d topic=%s msg_id=%s\n", sent, topic, msg.msg_id);
                }
            } else {
                queue_event(&ctx, topic, json);
            }
        } else {
            queue_event(&ctx, topic, json);
        }

        // 중복 재전송
        if (cfg.dup_inject && ctx.connected) {
            mark_message_as_dup(&msg);
            std::string dup_json = mqtt_message_to_json(msg);
            for (int d = 0; d < cfg.dup_count; d++) {
                mosquitto_publish(mosq, nullptr, topic, (int)dup_json.size(),
                                  dup_json.c_str(), cfg.qos, false);
                if (cfg.verbose) {
                    printf("[dup] #%d.%d topic=%s\n", sent, d + 1, topic);
                }
            }
        }

        if (sleep_us > 0) {
            usleep((useconds_t)sleep_us);
        }
    }

    auto t_end   = std::chrono::steady_clock::now();
    double elapsed_ms = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_end - t_start).count();
    double throughput = elapsed_ms > 0 ? (sent / (elapsed_ms / 1000.0)) : 0.0;

    // 남은 패킷 flush
    flush_store_queue(&ctx);
    usleep(200000);  // 200ms

    printf("\n[pub_sim] 완료\n");
    printf("  전송: %d 이벤트\n", sent);
    printf("  시간: %.1f ms\n", elapsed_ms);
    printf("  처리량: %.1f events/sec\n", throughput);
    {
        std::lock_guard<std::mutex> lock(ctx.queue_mutex);
        if (!ctx.store_queue.empty()) {
            printf("  미전송 큐: %zu 이벤트\n", ctx.store_queue.size());
        }
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
