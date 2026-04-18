// test_publisher_logic.cpp
// Publisher 헬퍼 함수 단위 테스트: build_event_topic, build_event_message,
//                                   next_event_type, msg_type_to_topic_segment,
//                                   mark_message_as_dup, rate_to_sleep_us,
//                                   parse_publisher_args
//
// 외부 프레임워크 없음 — mosquitto 의존 없이 순수 로직만 테스트

#include <cstdio>
#include <cstring>
#include <string>
#include "publisher_helpers.h"
#include "mqtt_json.h"

// ── 미니 테스트 러너 ──────────────────────────────────────────────────────────

static int  g_pass = 0;
static int  g_fail = 0;
static bool g_test_ok = true;

static void begin_test(const char* name) {
    g_test_ok = true;
    printf("[ RUN  ] %s\n", name);
}

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            g_fail++; g_test_ok = false; \
            fprintf(stderr, "        FAIL  %s:%d  (%s)\n", __FILE__, __LINE__, #expr); \
        } else { \
            g_pass++; \
        } \
    } while (0)

#define CHECK_EQ(a, b)    CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(std::strcmp((a), (b)) == 0)
#define CHECK_TRUE(e)     CHECK(e)
#define CHECK_FALSE(e)    CHECK(!(e))

static void end_test(const char* name) {
    printf("[  %s ] %s\n\n", g_test_ok ? " OK " : "FAIL", name);
}

// ── 테스트 상수 ───────────────────────────────────────────────────────────────

#define TEST_PUB_ID "aaaaaaaa-1234-4000-8000-000000000001"

// ── TC-01: build_event_topic — 정상 topic 생성 ───────────────────────────────

static void tc_build_event_topic_normal() {
    begin_test("TC-01: build_event_topic — 정상 topic 생성");

    char buf[256];

    CHECK_TRUE(build_event_topic("motion", "building-a", "cam-01", buf, sizeof(buf)));
    CHECK_STREQ(buf, "campus/data/motion/building-a/cam-01");

    CHECK_TRUE(build_event_topic("intrusion", "bldg-b", "cam-99", buf, sizeof(buf)));
    CHECK_STREQ(buf, "campus/data/intrusion/bldg-b/cam-99");

    CHECK_TRUE(build_event_topic("door", "hall", "entrance", buf, sizeof(buf)));
    CHECK_STREQ(buf, "campus/data/door/hall/entrance");

    end_test("TC-01: build_event_topic — 정상 topic 생성");
}

// ── TC-02: build_event_topic — 빈/null 인자 → false ─────────────────────────

static void tc_build_event_topic_invalid() {
    begin_test("TC-02: build_event_topic — 빈/null 인자 → false");

    char buf[256];

    // type_str 비어있음
    CHECK_FALSE(build_event_topic("", "building-a", "cam-01", buf, sizeof(buf)));
    // type_str null
    CHECK_FALSE(build_event_topic(nullptr, "building-a", "cam-01", buf, sizeof(buf)));
    // building 비어있음
    CHECK_FALSE(build_event_topic("motion", "", "cam-01", buf, sizeof(buf)));
    // camera 비어있음
    CHECK_FALSE(build_event_topic("motion", "building-a", "", buf, sizeof(buf)));
    // out null
    CHECK_FALSE(build_event_topic("motion", "building-a", "cam-01", nullptr, sizeof(buf)));

    end_test("TC-02: build_event_topic — 빈/null 인자 → false");
}

// ── TC-03: build_event_message — MOTION 필드 확인 ────────────────────────────

static void tc_build_event_message_motion() {
    begin_test("TC-03: build_event_message — MOTION: type, priority(MEDIUM), qos 필드");

    MqttMessage msg;
    CHECK_TRUE(build_event_message(TEST_PUB_ID, MSG_TYPE_MOTION,
                                   "building-a", "cam-01", "sim-pub", 1, &msg));

    CHECK_EQ(msg.type,            MSG_TYPE_MOTION);
    CHECK_EQ(msg.priority,        PRIORITY_MEDIUM);
    CHECK_EQ(msg.delivery.qos,    1);
    CHECK_FALSE(msg.delivery.dup);
    CHECK_FALSE(msg.delivery.retain);
    CHECK_EQ(msg.route.hop_count, 0);
    CHECK_EQ(msg.route.ttl,       8);
    CHECK_EQ(msg.source.role,     NODE_ROLE_NODE);
    CHECK_STREQ(msg.source.id,    TEST_PUB_ID);
    CHECK_STREQ(msg.payload.building_id, "building-a");
    CHECK_STREQ(msg.payload.camera_id,   "cam-01");
    CHECK_STREQ(msg.payload.description, "sim-pub");

    // msg_id 는 UUID 형식 (36자)
    CHECK_EQ((int)std::strlen(msg.msg_id), 36);

    // timestamp 는 비어있지 않아야 함
    CHECK_TRUE(msg.timestamp[0] != '\0');

    end_test("TC-03: build_event_message — MOTION: type, priority(MEDIUM), qos 필드");
}

// ── TC-04: build_event_message — INTRUSION priority(HIGH) ────────────────────

static void tc_build_event_message_intrusion() {
    begin_test("TC-04: build_event_message — INTRUSION: priority HIGH");

    MqttMessage msg;
    CHECK_TRUE(build_event_message(TEST_PUB_ID, MSG_TYPE_INTRUSION,
                                   "b1", "c1", "test", 0, &msg));
    CHECK_EQ(msg.type,     MSG_TYPE_INTRUSION);
    CHECK_EQ(msg.priority, PRIORITY_HIGH);
    CHECK_EQ(msg.delivery.qos, 0);

    end_test("TC-04: build_event_message — INTRUSION: priority HIGH");
}

// ── TC-05: build_event_message — DOOR_FORCED priority(HIGH) ──────────────────

static void tc_build_event_message_door() {
    begin_test("TC-05: build_event_message — DOOR_FORCED: priority HIGH");

    MqttMessage msg;
    CHECK_TRUE(build_event_message(TEST_PUB_ID, MSG_TYPE_DOOR_FORCED,
                                   "b2", "c2", "door-test", 2, &msg));
    CHECK_EQ(msg.type,     MSG_TYPE_DOOR_FORCED);
    CHECK_EQ(msg.priority, PRIORITY_HIGH);
    CHECK_EQ(msg.delivery.qos, 2);

    end_test("TC-05: build_event_message — DOOR_FORCED: priority HIGH");
}

// ── TC-06: build_event_message — publisher_id 비어있으면 false ───────────────

static void tc_build_event_message_empty_id() {
    begin_test("TC-06: build_event_message — publisher_id 비어있으면 false");

    MqttMessage msg;
    CHECK_FALSE(build_event_message("", MSG_TYPE_MOTION, "b", "c", "d", 1, &msg));
    CHECK_FALSE(build_event_message(nullptr, MSG_TYPE_MOTION, "b", "c", "d", 1, &msg));

    end_test("TC-06: build_event_message — publisher_id 비어있으면 false");
}

// ── TC-07: build_event_message → JSON round-trip ─────────────────────────────

static void tc_build_event_message_roundtrip() {
    begin_test("TC-07: build_event_message → JSON 역직렬화 round-trip");

    MqttMessage orig;
    CHECK_TRUE(build_event_message(TEST_PUB_ID, MSG_TYPE_INTRUSION,
                                   "bldg-a", "cam-01", "roundtrip-test", 1, &orig));

    std::string json = mqtt_message_to_json(orig);
    CHECK_FALSE(json.empty());

    MqttMessage parsed = {};
    CHECK_TRUE(mqtt_message_from_json(json, parsed));

    CHECK_STREQ(parsed.msg_id,           orig.msg_id);
    CHECK_EQ(parsed.type,                MSG_TYPE_INTRUSION);
    CHECK_EQ(parsed.priority,            PRIORITY_HIGH);
    CHECK_EQ(parsed.delivery.qos,        1);
    CHECK_STREQ(parsed.source.id,        TEST_PUB_ID);
    CHECK_STREQ(parsed.payload.building_id, "bldg-a");
    CHECK_STREQ(parsed.payload.camera_id,   "cam-01");

    end_test("TC-07: build_event_message → JSON 역직렬화 round-trip");
}

// ── TC-08: next_event_type — 세 비트 모두 set: 순환 확인 ─────────────────────

static void tc_next_event_type_all_bits() {
    begin_test("TC-08: next_event_type — 세 비트 모두 set: MOTION→DOOR→INTRUSION 순환");

    int mask = PUB_MASK_MOTION | PUB_MASK_DOOR | PUB_MASK_INTRUSION;
    int state = 0;

    CHECK_EQ(next_event_type(mask, &state), MSG_TYPE_MOTION);
    CHECK_EQ(next_event_type(mask, &state), MSG_TYPE_DOOR_FORCED);
    CHECK_EQ(next_event_type(mask, &state), MSG_TYPE_INTRUSION);
    // 순환 확인
    CHECK_EQ(next_event_type(mask, &state), MSG_TYPE_MOTION);
    CHECK_EQ(next_event_type(mask, &state), MSG_TYPE_DOOR_FORCED);

    end_test("TC-08: next_event_type — 세 비트 모두 set: MOTION→DOOR→INTRUSION 순환");
}

// ── TC-09: next_event_type — 단일 비트: 항상 같은 타입 ───────────────────────

static void tc_next_event_type_single() {
    begin_test("TC-09: next_event_type — 단일 비트: 항상 같은 타입");

    int state = 0;

    // INTRUSION만 활성화
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(next_event_type(PUB_MASK_INTRUSION, &state), MSG_TYPE_INTRUSION);
    }

    state = 0;
    // MOTION만 활성화
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(next_event_type(PUB_MASK_MOTION, &state), MSG_TYPE_MOTION);
    }

    state = 0;
    // DOOR만 활성화
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(next_event_type(PUB_MASK_DOOR, &state), MSG_TYPE_DOOR_FORCED);
    }

    end_test("TC-09: next_event_type — 단일 비트: 항상 같은 타입");
}

// ── TC-10: next_event_type — event_mask==0: MOTION fallback ──────────────────

static void tc_next_event_type_zero_mask() {
    begin_test("TC-10: next_event_type — event_mask==0: MOTION fallback");

    int state = 0;
    CHECK_EQ(next_event_type(0, &state), MSG_TYPE_MOTION);
    CHECK_EQ(next_event_type(0, &state), MSG_TYPE_MOTION);

    end_test("TC-10: next_event_type — event_mask==0: MOTION fallback");
}

// ── TC-11: msg_type_to_topic_segment — 세 타입 모두 ──────────────────────────

static void tc_msg_type_to_topic_segment() {
    begin_test("TC-11: msg_type_to_topic_segment — 세 타입 확인");

    CHECK_STREQ(msg_type_to_topic_segment(MSG_TYPE_MOTION),     "motion");
    CHECK_STREQ(msg_type_to_topic_segment(MSG_TYPE_DOOR_FORCED),"door");
    CHECK_STREQ(msg_type_to_topic_segment(MSG_TYPE_INTRUSION),  "intrusion");
    // 기타 타입 → "motion" (기본값)
    CHECK_STREQ(msg_type_to_topic_segment(MSG_TYPE_STATUS),     "motion");

    end_test("TC-11: msg_type_to_topic_segment — 세 타입 확인");
}

// ── TC-12: mark_message_as_dup — delivery.dup == true ────────────────────────

static void tc_mark_message_as_dup() {
    begin_test("TC-12: mark_message_as_dup — delivery.dup = true");

    MqttMessage msg = {};
    CHECK_FALSE(msg.delivery.dup);

    mark_message_as_dup(&msg);
    CHECK_TRUE(msg.delivery.dup);

    // nullptr 에 대해 crash 없이 동작
    mark_message_as_dup(nullptr);

    end_test("TC-12: mark_message_as_dup — delivery.dup = true");
}

// ── TC-13: rate_to_sleep_us — Hz → usec 변환 ─────────────────────────────────

static void tc_rate_to_sleep_us() {
    begin_test("TC-13: rate_to_sleep_us — 1Hz→1000000, 10Hz→100000, 0Hz→0");

    CHECK_EQ(rate_to_sleep_us(1),   1000000L);
    CHECK_EQ(rate_to_sleep_us(10),   100000L);
    CHECK_EQ(rate_to_sleep_us(100),   10000L);
    CHECK_EQ(rate_to_sleep_us(1000),   1000L);
    CHECK_EQ(rate_to_sleep_us(0),         0L);
    CHECK_EQ(rate_to_sleep_us(-1),        0L);

    end_test("TC-13: rate_to_sleep_us — 1Hz→1000000, 10Hz→100000, 0Hz→0");
}

// ── TC-14: parse_publisher_args — 기본값 확인 ────────────────────────────────

static void tc_parse_args_defaults() {
    begin_test("TC-14: parse_publisher_args — 기본값 확인");

    char prog[] = "pub_sim";
    char* argv[] = { prog };
    PublisherConfig cfg;
    CHECK_TRUE(parse_publisher_args(1, argv, &cfg));

    CHECK_STREQ(cfg.broker_host,  "localhost");
    CHECK_EQ(cfg.broker_port,     1883);
    CHECK_EQ(cfg.count,           10);
    CHECK_EQ(cfg.rate_hz,         1);
    CHECK_EQ(cfg.qos,             1);
    CHECK_FALSE(cfg.dup_inject);
    CHECK_EQ(cfg.dup_count,       1);
    CHECK_FALSE(cfg.burst_mode);
    CHECK_FALSE(cfg.register_edge);
    CHECK_EQ(cfg.event_mask, PUB_MASK_MOTION | PUB_MASK_DOOR | PUB_MASK_INTRUSION);
    CHECK_FALSE(cfg.multi_pub);
    CHECK_FALSE(cfg.verbose);
    CHECK_STREQ(cfg.building_id,  "building-a");
    CHECK_STREQ(cfg.camera_id,    "cam-01");
    CHECK_STREQ(cfg.description,  "sim-pub");
    // publisher_id 는 auto-generated UUID (36자)
    CHECK_EQ((int)std::strlen(cfg.publisher_id), 36);

    end_test("TC-14: parse_publisher_args — 기본값 확인");
}

// ── TC-15: parse_publisher_args — 주요 인자 파싱 ─────────────────────────────

static void tc_parse_args_values() {
    begin_test("TC-15: parse_publisher_args — --host, --port, --count, --rate, --qos 파싱");

    char prog[]    = "pub_sim";
    char host[]    = "--host";
    char hostval[] = "192.168.1.10";
    char port[]    = "--port";
    char portval[] = "1884";
    char count[]   = "--count";
    char countval[]= "500";
    char rate[]    = "--rate";
    char rateval[] = "50";
    char qos[]     = "--qos";
    char qosval[]  = "0";

    char* argv[] = { prog, host, hostval, port, portval,
                     count, countval, rate, rateval, qos, qosval };
    int   argc   = 11;

    PublisherConfig cfg;
    CHECK_TRUE(parse_publisher_args(argc, argv, &cfg));
    CHECK_STREQ(cfg.broker_host, "192.168.1.10");
    CHECK_EQ(cfg.broker_port,    1884);
    CHECK_EQ(cfg.count,          500);
    CHECK_EQ(cfg.rate_hz,        50);
    CHECK_EQ(cfg.qos,            0);

    end_test("TC-15: parse_publisher_args — --host, --port, --count, --rate, --qos 파싱");
}

// ── TC-16: parse_publisher_args — --burst, --dup 플래그 ──────────────────────

static void tc_parse_args_flags() {
    begin_test("TC-16: parse_publisher_args — --burst, --dup 플래그");

    // --burst
    {
        char prog[]  = "pub_sim";
        char burst[] = "--burst";
        char* argv[] = { prog, burst };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(2, argv, &cfg));
        CHECK_TRUE(cfg.burst_mode);
    }

    // --dup (횟수 없음 → dup_count 기본값 1)
    {
        char prog[] = "pub_sim";
        char dup[]  = "--dup";
        char* argv[] = { prog, dup };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(2, argv, &cfg));
        CHECK_TRUE(cfg.dup_inject);
        CHECK_EQ(cfg.dup_count, 1);
    }

    // --dup 3 (횟수 명시)
    {
        char prog[]   = "pub_sim";
        char dup[]    = "--dup";
        char dupval[] = "3";
        char* argv[] = { prog, dup, dupval };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(3, argv, &cfg));
        CHECK_TRUE(cfg.dup_inject);
        CHECK_EQ(cfg.dup_count, 3);
    }

    // --register, --multi-pub, --verbose
    {
        char prog[]     = "pub_sim";
        char reg[]      = "--register";
        char multi[]    = "--multi-pub";
        char verbose[]  = "--verbose";
        char* argv[] = { prog, reg, multi, verbose };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(4, argv, &cfg));
        CHECK_TRUE(cfg.register_edge);
        CHECK_TRUE(cfg.multi_pub);
        CHECK_TRUE(cfg.verbose);
    }

    end_test("TC-16: parse_publisher_args — --burst, --dup 플래그");
}

// ── TC-17: parse_publisher_args — 잘못된 port → false ───────────────────────

static void tc_parse_args_invalid_port() {
    begin_test("TC-17: parse_publisher_args — 잘못된 port(0, 65536) → false");

    // port = 0
    {
        char prog[]    = "pub_sim";
        char port[]    = "--port";
        char portval[] = "0";
        char* argv[] = { prog, port, portval };
        PublisherConfig cfg;
        CHECK_FALSE(parse_publisher_args(3, argv, &cfg));
    }

    // port = 65536
    {
        char prog[]    = "pub_sim";
        char port[]    = "--port";
        char portval[] = "65536";
        char* argv[] = { prog, port, portval };
        PublisherConfig cfg;
        CHECK_FALSE(parse_publisher_args(3, argv, &cfg));
    }

    end_test("TC-17: parse_publisher_args — 잘못된 port(0, 65536) → false");
}

// ── TC-18: parse_publisher_args — --events 파싱 ──────────────────────────────

static void tc_parse_args_events() {
    begin_test("TC-18: parse_publisher_args — --events motion,door → event_mask");

    // motion,door → bit0 | bit1
    {
        char prog[]       = "pub_sim";
        char events[]     = "--events";
        char eventsval[]  = "motion,door";
        char* argv[] = { prog, events, eventsval };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(3, argv, &cfg));
        CHECK_EQ(cfg.event_mask, PUB_MASK_MOTION | PUB_MASK_DOOR);
    }

    // intrusion only → bit2
    {
        char prog[]       = "pub_sim";
        char events[]     = "--events";
        char eventsval[]  = "intrusion";
        char* argv[] = { prog, events, eventsval };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(3, argv, &cfg));
        CHECK_EQ(cfg.event_mask, PUB_MASK_INTRUSION);
    }

    // motion,door,intrusion → 7 (all)
    {
        char prog[]       = "pub_sim";
        char events[]     = "--events";
        char eventsval[]  = "motion,door,intrusion";
        char* argv[] = { prog, events, eventsval };
        PublisherConfig cfg;
        CHECK_TRUE(parse_publisher_args(3, argv, &cfg));
        CHECK_EQ(cfg.event_mask, PUB_MASK_MOTION | PUB_MASK_DOOR | PUB_MASK_INTRUSION);
    }

    end_test("TC-18: parse_publisher_args — --events motion,door → event_mask");
}

// ── TC-19: select_fallback_broker — ONLINE Edge 중 RTT 최소 선택 ─────────────

static void tc_select_fallback_rtt_min() {
    begin_test("TC-19: select_fallback_broker — ONLINE Edge 중 RTT 최소 선택");

    ConnectionTable ct = {};
    ct.version = 1;

    // primary edge
    NodeEntry primary = {};
    std::strncpy(primary.id, "edge-primary-0001", UUID_LEN - 1);
    primary.role = NODE_ROLE_NODE;
    std::strncpy(primary.ip, "10.0.0.1", IP_LEN - 1);
    primary.port = 1883;
    primary.status = NODE_STATUS_OFFLINE;
    primary.hop_to_core = 1;
    ct.nodes[ct.node_count++] = primary;

    // Edge A: RTT=50ms
    NodeEntry edgeA = {};
    std::strncpy(edgeA.id, "edge-aaaa-0001", UUID_LEN - 1);
    edgeA.role = NODE_ROLE_NODE;
    std::strncpy(edgeA.ip, "10.0.0.2", IP_LEN - 1);
    edgeA.port = 1883;
    edgeA.status = NODE_STATUS_ONLINE;
    edgeA.hop_to_core = 1;
    ct.nodes[ct.node_count++] = edgeA;

    // Edge B: RTT=20ms (더 빠름)
    NodeEntry edgeB = {};
    std::strncpy(edgeB.id, "edge-bbbb-0001", UUID_LEN - 1);
    edgeB.role = NODE_ROLE_NODE;
    std::strncpy(edgeB.ip, "10.0.0.3", IP_LEN - 1);
    edgeB.port = 1883;
    edgeB.status = NODE_STATUS_ONLINE;
    edgeB.hop_to_core = 1;
    ct.nodes[ct.node_count++] = edgeB;

    // Links from primary
    LinkEntry linkA = {};
    std::strncpy(linkA.from_id, "edge-primary-0001", UUID_LEN - 1);
    std::strncpy(linkA.to_id, "edge-aaaa-0001", UUID_LEN - 1);
    linkA.rtt_ms = 50.0f;
    ct.links[ct.link_count++] = linkA;

    LinkEntry linkB = {};
    std::strncpy(linkB.from_id, "edge-primary-0001", UUID_LEN - 1);
    std::strncpy(linkB.to_id, "edge-bbbb-0001", UUID_LEN - 1);
    linkB.rtt_ms = 20.0f;
    ct.links[ct.link_count++] = linkB;

    FallbackBroker fb = select_fallback_broker(ct, "edge-primary-0001");
    CHECK_TRUE(fb.found);
    CHECK_STREQ(fb.id, "edge-bbbb-0001");
    CHECK_STREQ(fb.ip, "10.0.0.3");
    CHECK_EQ(fb.port, 1883);

    end_test("TC-19: select_fallback_broker — ONLINE Edge 중 RTT 최소 선택");
}

// ── TC-20: select_fallback_broker — RTT 동점 시 hop_to_core 최소 선택 ────────

static void tc_select_fallback_hop_tiebreaker() {
    begin_test("TC-20: select_fallback_broker — RTT 동점 시 hop_to_core 최소 선택");

    ConnectionTable ct = {};
    ct.version = 1;

    // Edge A: RTT=30ms, hop=2
    NodeEntry edgeA = {};
    std::strncpy(edgeA.id, "edge-aaaa-0002", UUID_LEN - 1);
    edgeA.role = NODE_ROLE_NODE;
    std::strncpy(edgeA.ip, "10.0.0.2", IP_LEN - 1);
    edgeA.port = 2883;
    edgeA.status = NODE_STATUS_ONLINE;
    edgeA.hop_to_core = 2;
    ct.nodes[ct.node_count++] = edgeA;

    // Edge B: RTT=30ms, hop=1 (더 가까움)
    NodeEntry edgeB = {};
    std::strncpy(edgeB.id, "edge-bbbb-0002", UUID_LEN - 1);
    edgeB.role = NODE_ROLE_NODE;
    std::strncpy(edgeB.ip, "10.0.0.3", IP_LEN - 1);
    edgeB.port = 3883;
    edgeB.status = NODE_STATUS_ONLINE;
    edgeB.hop_to_core = 1;
    ct.nodes[ct.node_count++] = edgeB;

    LinkEntry linkA = {};
    std::strncpy(linkA.from_id, "edge-primary-x", UUID_LEN - 1);
    std::strncpy(linkA.to_id, "edge-aaaa-0002", UUID_LEN - 1);
    linkA.rtt_ms = 30.0f;
    ct.links[ct.link_count++] = linkA;

    LinkEntry linkB = {};
    std::strncpy(linkB.from_id, "edge-primary-x", UUID_LEN - 1);
    std::strncpy(linkB.to_id, "edge-bbbb-0002", UUID_LEN - 1);
    linkB.rtt_ms = 30.0f;
    ct.links[ct.link_count++] = linkB;

    FallbackBroker fb = select_fallback_broker(ct, "edge-primary-x");
    CHECK_TRUE(fb.found);
    CHECK_STREQ(fb.id, "edge-bbbb-0002");
    CHECK_EQ(fb.port, 3883);

    end_test("TC-20: select_fallback_broker — RTT 동점 시 hop_to_core 최소 선택");
}

// ── TC-21: select_fallback_broker — ONLINE Edge 없으면 Core 선택 ─────────────

static void tc_select_fallback_core_when_no_edge() {
    begin_test("TC-21: select_fallback_broker — ONLINE Edge 없으면 ONLINE Core 반환");

    ConnectionTable ct = {};
    ct.version = 1;

    // OFFLINE Edge
    NodeEntry edgeOff = {};
    std::strncpy(edgeOff.id, "edge-off-0001", UUID_LEN - 1);
    edgeOff.role = NODE_ROLE_NODE;
    std::strncpy(edgeOff.ip, "10.0.0.2", IP_LEN - 1);
    edgeOff.port = 1883;
    edgeOff.status = NODE_STATUS_OFFLINE;
    edgeOff.hop_to_core = 1;
    ct.nodes[ct.node_count++] = edgeOff;

    // ONLINE Core
    NodeEntry core = {};
    std::strncpy(core.id, "core-active-0001", UUID_LEN - 1);
    core.role = NODE_ROLE_CORE;
    std::strncpy(core.ip, "10.0.0.100", IP_LEN - 1);
    core.port = 1883;
    core.status = NODE_STATUS_ONLINE;
    core.hop_to_core = 0;
    ct.nodes[ct.node_count++] = core;

    FallbackBroker fb = select_fallback_broker(ct, "edge-primary-y");
    CHECK_TRUE(fb.found);
    CHECK_STREQ(fb.id, "core-active-0001");
    CHECK_STREQ(fb.ip, "10.0.0.100");
    CHECK_EQ(fb.port, 1883);

    end_test("TC-21: select_fallback_broker — ONLINE Edge 없으면 ONLINE Core 반환");
}

// ── TC-22: select_fallback_broker — 모두 OFFLINE이면 found=false ─────────────

static void tc_select_fallback_all_offline() {
    begin_test("TC-22: select_fallback_broker — 모두 OFFLINE이면 found=false");

    ConnectionTable ct = {};
    ct.version = 1;

    NodeEntry offA = {};
    std::strncpy(offA.id, "edge-off-a", UUID_LEN - 1);
    offA.role = NODE_ROLE_NODE;
    std::strncpy(offA.ip, "10.0.0.2", IP_LEN - 1);
    offA.port = 1883;
    offA.status = NODE_STATUS_OFFLINE;
    ct.nodes[ct.node_count++] = offA;

    NodeEntry offB = {};
    std::strncpy(offB.id, "core-off-b", UUID_LEN - 1);
    offB.role = NODE_ROLE_CORE;
    std::strncpy(offB.ip, "10.0.0.100", IP_LEN - 1);
    offB.port = 1883;
    offB.status = NODE_STATUS_OFFLINE;
    ct.nodes[ct.node_count++] = offB;

    FallbackBroker fb = select_fallback_broker(ct, "edge-primary-z");
    CHECK_FALSE(fb.found);

    end_test("TC-22: select_fallback_broker — 모두 OFFLINE이면 found=false");
}

// ── TC-23: select_fallback_broker — primary edge 자체는 후보 제외 ────────────

static void tc_select_fallback_excludes_primary() {
    begin_test("TC-23: select_fallback_broker — primary edge 자체는 후보에서 제외");

    ConnectionTable ct = {};
    ct.version = 1;

    // primary edge ONLINE — 후보에서 제외되어야 함
    NodeEntry primary = {};
    std::strncpy(primary.id, "edge-primary-only", UUID_LEN - 1);
    primary.role = NODE_ROLE_NODE;
    std::strncpy(primary.ip, "10.0.0.1", IP_LEN - 1);
    primary.port = 1883;
    primary.status = NODE_STATUS_ONLINE;
    primary.hop_to_core = 1;
    ct.nodes[ct.node_count++] = primary;

    // 다른 후보 없음
    FallbackBroker fb = select_fallback_broker(ct, "edge-primary-only");
    CHECK_FALSE(fb.found);

    end_test("TC-23: select_fallback_broker — primary edge 자체는 후보에서 제외");
}

// ── TC-24: should_return_to_primary — ONLINE이면 true ────────────────────────

static void tc_return_to_primary_online() {
    begin_test("TC-24: should_return_to_primary — primary edge ONLINE이면 true");

    ConnectionTable ct = {};
    ct.version = 1;

    NodeEntry primary = {};
    std::strncpy(primary.id, "edge-primary-ret", UUID_LEN - 1);
    primary.role = NODE_ROLE_NODE;
    primary.status = NODE_STATUS_ONLINE;
    ct.nodes[ct.node_count++] = primary;

    CHECK_TRUE(should_return_to_primary(ct, "edge-primary-ret"));

    end_test("TC-24: should_return_to_primary — primary edge ONLINE이면 true");
}

// ── TC-25: should_return_to_primary — OFFLINE이면 false ──────────────────────

static void tc_return_to_primary_offline() {
    begin_test("TC-25: should_return_to_primary — primary edge OFFLINE이면 false");

    ConnectionTable ct = {};
    ct.version = 1;

    NodeEntry primary = {};
    std::strncpy(primary.id, "edge-primary-off", UUID_LEN - 1);
    primary.role = NODE_ROLE_NODE;
    primary.status = NODE_STATUS_OFFLINE;
    ct.nodes[ct.node_count++] = primary;

    CHECK_FALSE(should_return_to_primary(ct, "edge-primary-off"));

    end_test("TC-25: should_return_to_primary — primary edge OFFLINE이면 false");
}

// ── TC-26: should_return_to_primary — CT에 없으면 false ──────────────────────

static void tc_return_to_primary_missing() {
    begin_test("TC-26: should_return_to_primary — primary edge가 CT에 없으면 false");

    ConnectionTable ct = {};
    ct.version = 1;
    // 빈 CT

    CHECK_FALSE(should_return_to_primary(ct, "edge-nonexistent"));
    // null/empty edge_id
    CHECK_FALSE(should_return_to_primary(ct, nullptr));
    CHECK_FALSE(should_return_to_primary(ct, ""));

    end_test("TC-26: should_return_to_primary — primary edge가 CT에 없으면 false");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("════════════════════════════════════════════════════\n");
    printf(" Publisher Logic Tests\n");
    printf("════════════════════════════════════════════════════\n\n");

    tc_build_event_topic_normal();
    tc_build_event_topic_invalid();
    tc_build_event_message_motion();
    tc_build_event_message_intrusion();
    tc_build_event_message_door();
    tc_build_event_message_empty_id();
    tc_build_event_message_roundtrip();
    tc_next_event_type_all_bits();
    tc_next_event_type_single();
    tc_next_event_type_zero_mask();
    tc_msg_type_to_topic_segment();
    tc_mark_message_as_dup();
    tc_rate_to_sleep_us();
    tc_parse_args_defaults();
    tc_parse_args_values();
    tc_parse_args_flags();
    tc_parse_args_invalid_port();
    tc_parse_args_events();

    // Phase 7: CT 기반 Failover 단위 테스트
    tc_select_fallback_rtt_min();
    tc_select_fallback_hop_tiebreaker();
    tc_select_fallback_core_when_no_edge();
    tc_select_fallback_all_offline();
    tc_select_fallback_excludes_primary();
    tc_return_to_primary_online();
    tc_return_to_primary_offline();
    tc_return_to_primary_missing();

    printf("══════════════════════════════════════\n");
    printf("  결과: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════\n");
    return g_fail == 0 ? 0 : 1;
}
