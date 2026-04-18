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
//   - 종료 시 처리량 요약 출력

#include <cstdio>
#include <cstring>
#include <csignal>
#include <chrono>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#  define usleep(us) Sleep((us) / 1000)
#else
#  include <unistd.h>
#endif

#include <mosquitto.h>

#include "publisher_helpers.h"
#include "mqtt_json.h"

// ── 시그널 처리 ───────────────────────────────────────────────────────────────

static volatile bool g_stop = false;

static void on_signal(int) { g_stop = true; }

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

    // ── mosquitto 초기화 ──────────────────────────────────────────────────────
    mosquitto_lib_init();

    struct mosquitto* mosq = mosquitto_new(cfg.publisher_id, true, nullptr);
    if (!mosq) {
        fprintf(stderr, "오류: mosquitto_new 실패\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    if (mosquitto_connect(mosq, cfg.broker_host, cfg.broker_port, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "오류: 브로커 연결 실패 (%s:%d)\n",
                cfg.broker_host, cfg.broker_port);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    printf("[pub_sim] 브로커 %s:%d 연결 완료 (id=%s)\n",
           cfg.broker_host, cfg.broker_port, cfg.publisher_id);

    // ── Edge 등록 ─────────────────────────────────────────────────────────────
    if (cfg.register_edge) {
        publish_registration(mosq, cfg.publisher_id, cfg.qos);
        mosquitto_loop(mosq, 100, 1);
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

        char topic[256];
        if (!build_event_topic(type_str, cfg.building_id, cfg.camera_id,
                                topic, sizeof(topic))) {
            continue;
        }

        std::string json = mqtt_message_to_json(msg);

        // 1차 전송
        mosquitto_publish(mosq, nullptr, topic, (int)json.size(),
                          json.c_str(), cfg.qos, false);
        sent++;

        if (cfg.verbose) {
            printf("[pub] #%d topic=%s msg_id=%s\n", sent, topic, msg.msg_id);
        }

        // 중복 재전송
        if (cfg.dup_inject) {
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

        // mosquitto 내부 루프 (ACK 처리)
        mosquitto_loop(mosq, 0, 1);

        if (sleep_us > 0) {
            usleep((useconds_t)sleep_us);
        }
    }

    auto t_end   = std::chrono::steady_clock::now();
    double elapsed_ms = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_end - t_start).count();
    double throughput = elapsed_ms > 0 ? (sent / (elapsed_ms / 1000.0)) : 0.0;

    // 남은 패킷 flush
    mosquitto_loop(mosq, 200, 1);

    printf("\n[pub_sim] 완료\n");
    printf("  전송: %d 이벤트\n", sent);
    printf("  시간: %.1f ms\n", elapsed_ms);
    printf("  처리량: %.1f events/sec\n", throughput);

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
