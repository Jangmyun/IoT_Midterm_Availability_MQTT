// test_json.cpp
// ConnectionTable / MqttMessage 직렬화·역직렬화 단위 테스트
//
// 테스트 데이터는 test_pub.sh 의 ID 상수 및 JSON 페이로드와 동일한 값 사용
// 외부 프레임워크 없음 — 빌드 후 ./build/test_json 으로 실행

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
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

#define CHECK_EQ(a, b)            CHECK((a) == (b))
#define CHECK_STREQ(a, b)         CHECK(std::strcmp((a), (b)) == 0)
#define CHECK_FLOAT_NEAR(a, b, e) CHECK(std::fabs((double)(a) - (double)(b)) < (e))
#define CHECK_TRUE(e)             CHECK(e)
#define CHECK_FALSE(e)            CHECK(!(e))

static void end_test(const char* name) {
    printf("[  %s ] %s\n\n", g_test_ok ? " OK " : "FAIL", name);
}

// ── 테스트 상수 — test_pub.sh 와 동일 ─────────────────────────────────────────
#define CORE_A  "aaaaaaaa-0000-0000-0000-000000000001"
#define CORE_B  "bbbbbbbb-0000-0000-0000-000000000002"
#define NODE_1  "cccccccc-0000-0000-0000-000000000003"
#define NODE_2  "dddddddd-0000-0000-0000-000000000004"
#define MSG_EVT "550e8400-e29b-41d4-a716-446655440000"

// ── 헬퍼 ──────────────────────────────────────────────────────────────────────

static NodeEntry make_node(const char* id, NodeRole role, const char* ip,
                            uint16_t port, NodeStatus status, int hop) {
    NodeEntry n = {};
    std::strncpy(n.id, id, UUID_LEN - 1);
    n.role = role;
    std::strncpy(n.ip, ip, IP_LEN - 1);
    n.port       = port;
    n.status     = status;
    n.hop_to_core = hop;
    return n;
}

static LinkEntry make_link(const char* from, const char* to, float rtt) {
    LinkEntry l = {};
    std::strncpy(l.from_id, from, UUID_LEN - 1);
    std::strncpy(l.to_id,   to,   UUID_LEN - 1);
    l.rtt_ms = rtt;
    return l;
}

// test_pub.sh send_topology 와 동일한 4-node / 3-link 구성
static ConnectionTable make_test_ct() {
    ConnectionTable ct = {};
    ct.version = 1;
    std::strncpy(ct.last_update,    "2026-04-13T12:00:00Z", TIMESTAMP_LEN - 1);
    std::strncpy(ct.active_core_id, CORE_A,                 UUID_LEN - 1);
    std::strncpy(ct.backup_core_id, CORE_B,                 UUID_LEN - 1);

    ct.nodes[ct.node_count++] = make_node(CORE_A, NODE_ROLE_CORE, "127.0.0.1", 1883, NODE_STATUS_ONLINE, 0);
    ct.nodes[ct.node_count++] = make_node(CORE_B, NODE_ROLE_CORE, "127.0.0.2", 1883, NODE_STATUS_ONLINE, 1);
    ct.nodes[ct.node_count++] = make_node(NODE_1, NODE_ROLE_NODE, "10.0.0.3",  1883, NODE_STATUS_ONLINE, 2);
    ct.nodes[ct.node_count++] = make_node(NODE_2, NODE_ROLE_NODE, "10.0.0.4",  1883, NODE_STATUS_ONLINE, 2);

    ct.links[ct.link_count++] = make_link(CORE_A, CORE_B, 1.2f);
    ct.links[ct.link_count++] = make_link(CORE_A, NODE_1, 4.7f);
    ct.links[ct.link_count++] = make_link(CORE_A, NODE_2, 8.1f);

    return ct;
}

// ── ConnectionTable 테스트 ────────────────────────────────────────────────────

static void test_ct_roundtrip() {
    const char* name = "CT: serialize → deserialize (round-trip)";
    begin_test(name);

    ConnectionTable orig   = make_test_ct();
    std::string     json   = connection_table_to_json(orig);
    ConnectionTable parsed = {};
    CHECK_TRUE(connection_table_from_json(json, parsed));

    CHECK_EQ(parsed.version,    1);
    CHECK_STREQ(parsed.last_update,    "2026-04-13T12:00:00Z");
    CHECK_STREQ(parsed.active_core_id, CORE_A);
    CHECK_STREQ(parsed.backup_core_id, CORE_B);
    CHECK_EQ(parsed.node_count, 4);
    CHECK_EQ(parsed.link_count, 3);

    // nodes[0]: CORE_A
    CHECK_STREQ(parsed.nodes[0].id, CORE_A);
    CHECK_EQ(parsed.nodes[0].role,  NODE_ROLE_CORE);
    CHECK_STREQ(parsed.nodes[0].ip, "127.0.0.1");
    CHECK_EQ(parsed.nodes[0].port,  1883);
    CHECK_EQ(parsed.nodes[0].status, NODE_STATUS_ONLINE);
    CHECK_EQ(parsed.nodes[0].hop_to_core, 0);

    // nodes[1]: CORE_B
    CHECK_STREQ(parsed.nodes[1].id, CORE_B);
    CHECK_EQ(parsed.nodes[1].hop_to_core, 1);

    // nodes[2]: NODE_1
    CHECK_STREQ(parsed.nodes[2].id, NODE_1);
    CHECK_EQ(parsed.nodes[2].role,  NODE_ROLE_NODE);
    CHECK_STREQ(parsed.nodes[2].ip, "10.0.0.3");
    CHECK_EQ(parsed.nodes[2].hop_to_core, 2);

    // links
    CHECK_STREQ(parsed.links[0].from_id, CORE_A);
    CHECK_STREQ(parsed.links[0].to_id,   CORE_B);
    CHECK_FLOAT_NEAR(parsed.links[0].rtt_ms, 1.2f, 0.01f);
    CHECK_FLOAT_NEAR(parsed.links[1].rtt_ms, 4.7f, 0.01f);
    CHECK_FLOAT_NEAR(parsed.links[2].rtt_ms, 8.1f, 0.01f);

    end_test(name);
}

static void test_ct_from_testpub_json() {
    // test_pub.sh send_topology 의 실제 JSON
    // node_count / link_count 필드 포함, last_update Z 없음 → 추가 필드는 무시되어야 함
    const char* name = "CT: parse test_pub.sh JSON (extra fields tolerated, no Z in last_update)";
    begin_test(name);

    // Adjacent string literal concatenation 으로 매크로 삽입
    const std::string raw =
        "{"
          "\"version\":1,"
          "\"last_update\":\"2026-04-13T12:00:00\","   // Z 없음 (test_pub.sh 현재 상태)
          "\"active_core_id\":\"" CORE_A "\","
          "\"backup_core_id\":\"" CORE_B "\","
          "\"node_count\":4,"                           // C++ 직렬화엔 없는 extra 필드
          "\"nodes\":["
            "{\"id\":\"" CORE_A "\",\"role\":\"CORE\",\"ip\":\"127.0.0.1\",\"port\":1883,\"status\":\"ONLINE\",\"hop_to_core\":0},"
            "{\"id\":\"" CORE_B "\",\"role\":\"CORE\",\"ip\":\"127.0.0.2\",\"port\":1883,\"status\":\"ONLINE\",\"hop_to_core\":1},"
            "{\"id\":\"" NODE_1 "\",\"role\":\"NODE\",\"ip\":\"10.0.0.3\", \"port\":1883,\"status\":\"ONLINE\",\"hop_to_core\":2},"
            "{\"id\":\"" NODE_2 "\",\"role\":\"NODE\",\"ip\":\"10.0.0.4\", \"port\":1883,\"status\":\"ONLINE\",\"hop_to_core\":2}"
          "],"
          "\"link_count\":3,"                           // extra 필드
          "\"links\":["
            "{\"from_id\":\"" CORE_A "\",\"to_id\":\"" CORE_B "\",\"rtt_ms\":1.2},"
            "{\"from_id\":\"" CORE_A "\",\"to_id\":\"" NODE_1 "\",\"rtt_ms\":4.7},"
            "{\"from_id\":\"" CORE_A "\",\"to_id\":\"" NODE_2 "\",\"rtt_ms\":8.1}"
          "]"
        "}";

    ConnectionTable ct = {};
    CHECK_TRUE(connection_table_from_json(raw, ct));
    CHECK_EQ(ct.version,    1);
    CHECK_STREQ(ct.active_core_id, CORE_A);
    CHECK_STREQ(ct.backup_core_id, CORE_B);
    CHECK_EQ(ct.node_count, 4);
    CHECK_EQ(ct.link_count, 3);
    CHECK_EQ(ct.nodes[0].role,   NODE_ROLE_CORE);
    CHECK_EQ(ct.nodes[2].role,   NODE_ROLE_NODE);
    CHECK_EQ(ct.nodes[2].hop_to_core, 2);
    CHECK_FLOAT_NEAR(ct.links[1].rtt_ms, 4.7f, 0.01f);

    end_test(name);
}

static void test_ct_offline_node() {
    const char* name = "CT: OFFLINE node status round-trip";
    begin_test(name);

    ConnectionTable ct = {};
    ct.version = 3;
    std::strncpy(ct.last_update,    "2026-04-14T09:00:00Z", TIMESTAMP_LEN - 1);
    std::strncpy(ct.active_core_id, CORE_A, UUID_LEN - 1);
    std::strncpy(ct.backup_core_id, CORE_B, UUID_LEN - 1);
    ct.nodes[ct.node_count++] = make_node(NODE_1, NODE_ROLE_NODE, "10.0.0.3", 1883, NODE_STATUS_OFFLINE, 2);

    std::string     json   = connection_table_to_json(ct);
    ConnectionTable parsed = {};
    CHECK_TRUE(connection_table_from_json(json, parsed));
    CHECK_EQ(parsed.nodes[0].status, NODE_STATUS_OFFLINE);
    CHECK_EQ(parsed.version, 3);

    end_test(name);
}

static void test_ct_invalid_json() {
    const char* name = "CT: invalid JSON → returns false";
    begin_test(name);

    ConnectionTable ct = {};
    CHECK_FALSE(connection_table_from_json("",          ct));
    CHECK_FALSE(connection_table_from_json("not json",  ct));
    CHECK_FALSE(connection_table_from_json("{}",        ct)); // 필수 필드 없음

    end_test(name);
}

static void test_ct_parse_clears_stale_bytes() {
    const char* name = "CT: parse clears stale bytes and null-terminates strings";
    begin_test(name);

    const std::string raw =
        "{"
          "\"version\":7,"
          "\"last_update\":\"2026-04-18T08:00:00Z\","
          "\"active_core_id\":\"" CORE_A "\","
          "\"backup_core_id\":\"" CORE_B "\","
          "\"nodes\":["
            "{\"id\":\"" NODE_1 "\",\"role\":\"NODE\",\"ip\":\"192.168.0.9\",\"port\":1883,\"status\":\"ONLINE\",\"hop_to_core\":1}"
          "],"
          "\"links\":["
            "{\"from_id\":\"" CORE_A "\",\"to_id\":\"" NODE_1 "\",\"rtt_ms\":1.0}"
          "]"
        "}";

    ConnectionTable ct;
    std::memset(&ct, 'X', sizeof(ct));
    CHECK_TRUE(connection_table_from_json(raw, ct));
    CHECK_STREQ(ct.nodes[0].id, NODE_1);
    CHECK_STREQ(ct.nodes[0].ip, "192.168.0.9");
    CHECK_EQ(ct.nodes[0].id[UUID_LEN - 1], '\0');
    CHECK_EQ(ct.nodes[0].ip[IP_LEN - 1], '\0');
    CHECK_EQ(ct.links[0].to_id[UUID_LEN - 1], '\0');

    end_test(name);
}

// ── MqttMessage 테스트 ────────────────────────────────────────────────────────

static void test_msg_intrusion_roundtrip() {
    // test_pub.sh send_event_intrusion 과 동일한 메시지 구성
    const char* name = "MqttMessage: INTRUSION HIGH round-trip";
    begin_test(name);

    MqttMessage msg = {};
    std::strncpy(msg.msg_id,    MSG_EVT,               UUID_LEN      - 1);
    std::strncpy(msg.timestamp, "2026-04-13T09:31:00Z", TIMESTAMP_LEN - 1);
    msg.type     = MSG_TYPE_INTRUSION;
    msg.priority = PRIORITY_HIGH;
    msg.source.role = NODE_ROLE_NODE;
    std::strncpy(msg.source.id, NODE_1, UUID_LEN - 1);
    msg.target.role = NODE_ROLE_CORE;
    std::strncpy(msg.target.id, CORE_A, UUID_LEN - 1);
    std::strncpy(msg.route.original_node, NODE_1, UUID_LEN - 1);
    std::strncpy(msg.route.prev_hop,      NODE_1, UUID_LEN - 1);
    std::strncpy(msg.route.next_hop,      CORE_A, UUID_LEN - 1);
    msg.route.hop_count = 1;
    msg.route.ttl       = 5;
    msg.delivery        = {1, false, false};
    std::strncpy(msg.payload.building_id, "bldg-a",              BUILDING_ID_LEN - 1);
    std::strncpy(msg.payload.camera_id,   "cam-01",              CAMERA_ID_LEN   - 1);
    std::strncpy(msg.payload.description, "침입 감지 — 정문 CCTV", DESCRIPTION_LEN - 1);

    std::string json   = mqtt_message_to_json(msg);
    MqttMessage parsed = {};
    CHECK_TRUE(mqtt_message_from_json(json, parsed));

    CHECK_STREQ(parsed.msg_id,    MSG_EVT);
    CHECK_EQ(parsed.type,         MSG_TYPE_INTRUSION);
    CHECK_EQ(parsed.priority,     PRIORITY_HIGH);
    CHECK_EQ(parsed.source.role,  NODE_ROLE_NODE);
    CHECK_STREQ(parsed.source.id, NODE_1);
    CHECK_EQ(parsed.target.role,  NODE_ROLE_CORE);
    CHECK_STREQ(parsed.target.id, CORE_A);
    CHECK_STREQ(parsed.route.original_node, NODE_1);
    CHECK_STREQ(parsed.route.next_hop,      CORE_A);
    CHECK_EQ(parsed.route.hop_count, 1);
    CHECK_EQ(parsed.route.ttl,       5);
    CHECK_EQ(parsed.delivery.qos,    1);
    CHECK_EQ(parsed.delivery.dup,    false);
    CHECK_EQ(parsed.delivery.retain, false);
    CHECK_STREQ(parsed.payload.building_id, "bldg-a");
    CHECK_STREQ(parsed.payload.camera_id,   "cam-01");
    CHECK_STREQ(parsed.payload.description, "침입 감지 — 정문 CCTV");

    end_test(name);
}

static void test_msg_motion_no_priority() {
    // test_pub.sh send_event_motion — priority 필드 없음 → JSON에 미포함 → PRIORITY_NONE
    const char* name = "MqttMessage: MOTION (no priority) — field omitted + PRIORITY_NONE round-trip";
    begin_test(name);

    MqttMessage msg = {};
    std::strncpy(msg.msg_id,    MSG_EVT,               UUID_LEN      - 1);
    std::strncpy(msg.timestamp, "2026-04-13T09:32:00Z", TIMESTAMP_LEN - 1);
    msg.type     = MSG_TYPE_MOTION;
    msg.priority = PRIORITY_NONE;  // 직렬화 시 필드 제외
    msg.source.role = NODE_ROLE_NODE;
    std::strncpy(msg.source.id, NODE_2, UUID_LEN - 1);
    msg.target.role = NODE_ROLE_CORE;
    std::strncpy(msg.target.id, CORE_A, UUID_LEN - 1);
    std::strncpy(msg.route.original_node, NODE_2, UUID_LEN - 1);
    std::strncpy(msg.route.prev_hop,      NODE_2, UUID_LEN - 1);
    std::strncpy(msg.route.next_hop,      CORE_A, UUID_LEN - 1);
    msg.route.hop_count = 1;
    msg.route.ttl       = 5;
    msg.delivery        = {1, false, false};
    std::strncpy(msg.payload.building_id, "bldg-b",     BUILDING_ID_LEN - 1);
    std::strncpy(msg.payload.camera_id,   "cam-05",     CAMERA_ID_LEN   - 1);
    std::strncpy(msg.payload.description, "복도 움직임 감지", DESCRIPTION_LEN - 1);

    std::string json = mqtt_message_to_json(msg);

    // PRIORITY_NONE 이면 JSON에 "priority" 키가 없어야 함
    CHECK_TRUE(json.find("\"priority\"") == std::string::npos);

    MqttMessage parsed = {};
    CHECK_TRUE(mqtt_message_from_json(json, parsed));
    CHECK_EQ(parsed.type,     MSG_TYPE_MOTION);
    CHECK_EQ(parsed.priority, PRIORITY_NONE);
    CHECK_STREQ(parsed.payload.building_id, "bldg-b");
    CHECK_STREQ(parsed.payload.description, "복도 움직임 감지");

    end_test(name);
}

static void test_msg_lwt_core_roundtrip() {
    // test_pub.sh send_lwt — LWT_CORE, description 에 backup core 정보
    const char* name = "MqttMessage: LWT_CORE round-trip";
    begin_test(name);

    MqttMessage msg = {};
    std::strncpy(msg.msg_id,    MSG_EVT,               UUID_LEN      - 1);
    std::strncpy(msg.timestamp, "2026-04-13T10:00:00Z", TIMESTAMP_LEN - 1);
    msg.type     = MSG_TYPE_LWT_CORE;
    msg.priority = PRIORITY_NONE;
    msg.source.role = NODE_ROLE_CORE;
    std::strncpy(msg.source.id, CORE_A, UUID_LEN - 1);
    msg.target.role = NODE_ROLE_CORE;
    std::strncpy(msg.target.id, CORE_B, UUID_LEN - 1);
    msg.delivery = {1, false, false};
    std::strncpy(msg.payload.description, "127.0.0.2:1883", DESCRIPTION_LEN - 1);

    std::string json   = mqtt_message_to_json(msg);
    MqttMessage parsed = {};
    CHECK_TRUE(mqtt_message_from_json(json, parsed));

    CHECK_EQ(parsed.type,          MSG_TYPE_LWT_CORE);
    CHECK_EQ(parsed.source.role,   NODE_ROLE_CORE);
    CHECK_STREQ(parsed.source.id,  CORE_A);
    CHECK_STREQ(parsed.target.id,  CORE_B);
    CHECK_STREQ(parsed.payload.description, "127.0.0.2:1883");

    end_test(name);
}

static void test_msg_from_testpub_json() {
    // test_pub.sh send_event_intrusion 과 동일한 raw JSON 문자열 파싱
    // msg_id 가 UUID 형식이 아닌 임의 문자열임에 주목 (test_pub.sh 현재 상태)
    const char* name = "MqttMessage: parse test_pub.sh INTRUSION JSON (non-UUID msg_id tolerated)";
    begin_test(name);

    const std::string raw =
        "{"
          "\"msg_id\":\"evt-a1b2c3d4\","           // UUID 형식 아님 — 파서는 허용해야 함
          "\"type\":\"INTRUSION\","
          "\"timestamp\":\"2026-04-13T09:31:00\","  // Z 없음
          "\"priority\":\"HIGH\","
          "\"source\":{\"role\":\"NODE\",\"id\":\"" NODE_1 "\"},"
          "\"target\":{\"role\":\"CORE\",\"id\":\"" CORE_A "\"},"
          "\"route\":{"
            "\"original_node\":\"" NODE_1 "\","
            "\"prev_hop\":\"" NODE_1 "\","
            "\"next_hop\":\"" CORE_A "\","
            "\"hop_count\":1,\"ttl\":5"
          "},"
          "\"delivery\":{\"qos\":1,\"dup\":false,\"retain\":false},"
          "\"payload\":{"
            "\"building_id\":\"bldg-a\","
            "\"camera_id\":\"cam-01\","
            "\"description\":\"침입 감지 — 정문 CCTV\""
          "}"
        "}";

    MqttMessage msg = {};
    CHECK_TRUE(mqtt_message_from_json(raw, msg));
    CHECK_STREQ(msg.msg_id,   "evt-a1b2c3d4");
    CHECK_EQ(msg.type,         MSG_TYPE_INTRUSION);
    CHECK_EQ(msg.priority,     PRIORITY_HIGH);
    CHECK_EQ(msg.source.role,  NODE_ROLE_NODE);
    CHECK_STREQ(msg.source.id, NODE_1);
    CHECK_EQ(msg.target.role,  NODE_ROLE_CORE);
    CHECK_STREQ(msg.target.id, CORE_A);
    CHECK_EQ(msg.route.hop_count, 1);
    CHECK_EQ(msg.route.ttl,       5);
    CHECK_STREQ(msg.payload.building_id, "bldg-a");
    CHECK_STREQ(msg.payload.description, "침입 감지 — 정문 CCTV");

    end_test(name);
}

static void test_msg_invalid_json() {
    const char* name = "MqttMessage: invalid JSON → returns false";
    begin_test(name);

    MqttMessage msg = {};
    CHECK_FALSE(mqtt_message_from_json("",         msg));
    CHECK_FALSE(mqtt_message_from_json("not json", msg));
    CHECK_FALSE(mqtt_message_from_json("{}",       msg)); // msg_id / type 없음

    end_test(name);
}

static void test_msg_parse_clears_stale_bytes() {
    const char* name = "MqttMessage: parse clears stale bytes and null-terminates strings";
    begin_test(name);

    const std::string raw =
        "{"
          "\"msg_id\":\"" MSG_EVT "\","
          "\"type\":\"STATUS\","
          "\"timestamp\":\"2026-04-18T08:10:00Z\","
          "\"source\":{\"role\":\"NODE\",\"id\":\"" NODE_1 "\"},"
          "\"target\":{\"role\":\"CORE\",\"id\":\"" CORE_A "\"},"
          "\"route\":{"
            "\"original_node\":\"" NODE_1 "\","
            "\"prev_hop\":\"" NODE_1 "\","
            "\"next_hop\":\"" CORE_A "\","
            "\"hop_count\":1,\"ttl\":5"
          "},"
          "\"delivery\":{\"qos\":1,\"dup\":false,\"retain\":false},"
          "\"payload\":{"
            "\"building_id\":\"bldg-a\","
            "\"camera_id\":\"cam-01\","
            "\"description\":\"192.168.0.9:1883\""
          "}"
        "}";

    MqttMessage msg;
    std::memset(&msg, 'Y', sizeof(msg));
    CHECK_TRUE(mqtt_message_from_json(raw, msg));
    CHECK_STREQ(msg.source.id, NODE_1);
    CHECK_STREQ(msg.target.id, CORE_A);
    CHECK_STREQ(msg.payload.description, "192.168.0.9:1883");
    CHECK_EQ(msg.source.id[UUID_LEN - 1], '\0');
    CHECK_EQ(msg.target.id[UUID_LEN - 1], '\0');
    CHECK_EQ(msg.payload.description[DESCRIPTION_LEN - 1], '\0');

    end_test(name);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("════════════════════════════════════════════════════\n");
    printf(" JSON Serialization Tests\n");
    printf("════════════════════════════════════════════════════\n\n");

    printf("── ConnectionTable ─────────────────────────────────\n\n");
    test_ct_roundtrip();
    test_ct_from_testpub_json();
    test_ct_offline_node();
    test_ct_invalid_json();
    test_ct_parse_clears_stale_bytes();

    printf("── MqttMessage ─────────────────────────────────────\n\n");
    test_msg_intrusion_roundtrip();
    test_msg_motion_no_priority();
    test_msg_lwt_core_roundtrip();
    test_msg_from_testpub_json();
    test_msg_invalid_json();
    test_msg_parse_clears_stale_bytes();

    printf("════════════════════════════════════════════════════\n");
    printf(" PASS: %d  /  FAIL: %d\n", g_pass, g_fail);
    printf("════════════════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
