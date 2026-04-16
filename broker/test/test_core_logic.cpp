// test_core_logic.cpp
// Core 로직 단위 테스트: parse_ip_port, make_alert_topic, msg_id 중복 필터
//
// 외부 프레임워크 없음 — 빌드 후 ./build/test_core_logic 으로 실행

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_set>
#include "core_helpers.h"
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

#define NODE_1  "cccccccc-0000-0000-0000-000000000003"
#define MSG_EVT "550e8400-e29b-41d4-a716-446655440000"

// ── TC-01: parse_ip_port 정상 케이스 ─────────────────────────────────────────

static void tc_parse_ip_port_normal() {
    begin_test("TC-01: parse_ip_port — 정상 케이스");

    char ip[64] = {};
    int  port = 0;

    CHECK_TRUE(parse_ip_port("192.168.1.10:1884", ip, sizeof(ip), &port));
    CHECK_STREQ(ip, "192.168.1.10");
    CHECK_EQ(port, 1884);

    port = 0;
    memset(ip, 0, sizeof(ip));
    CHECK_TRUE(parse_ip_port("127.0.0.1:1883", ip, sizeof(ip), &port));
    CHECK_STREQ(ip, "127.0.0.1");
    CHECK_EQ(port, 1883);

    port = 0;
    memset(ip, 0, sizeof(ip));
    CHECK_TRUE(parse_ip_port("10.0.0.1:9001", ip, sizeof(ip), &port));
    CHECK_STREQ(ip, "10.0.0.1");
    CHECK_EQ(port, 9001);

    end_test("TC-01: parse_ip_port — 정상 케이스");
}

// ── TC-02: parse_ip_port 비정상 케이스 ───────────────────────────────────────

static void tc_parse_ip_port_invalid() {
    begin_test("TC-02: parse_ip_port — 비정상 케이스");

    char ip[64] = {};
    int  port = 0;

    // 콜론 없음
    CHECK_FALSE(parse_ip_port("badstring", ip, sizeof(ip), &port));

    // 포트만 있고 ip 없음
    CHECK_FALSE(parse_ip_port(":1883", ip, sizeof(ip), &port));

    // 포트 0 (범위 밖)
    CHECK_FALSE(parse_ip_port("127.0.0.1:0", ip, sizeof(ip), &port));

    // 포트 65536 (범위 밖)
    CHECK_FALSE(parse_ip_port("127.0.0.1:65536", ip, sizeof(ip), &port));

    // 빈 문자열
    CHECK_FALSE(parse_ip_port("", ip, sizeof(ip), &port));

    end_test("TC-02: parse_ip_port — 비정상 케이스");
}

// ── TC-03: make_alert_topic ───────────────────────────────────────────────────

static void tc_make_alert_topic() {
    begin_test("TC-03: make_alert_topic");

    char buf[128] = {};

    make_alert_topic("campus/alert/node_down", NODE_1, buf, sizeof(buf));
    CHECK_STREQ(buf, "campus/alert/node_down/" NODE_1);

    make_alert_topic("campus/alert/node_up", NODE_1, buf, sizeof(buf));
    CHECK_STREQ(buf, "campus/alert/node_up/" NODE_1);

    end_test("TC-03: make_alert_topic");
}

// ── TC-04: msg_id 중복 필터 — 기본 동작 ──────────────────────────────────────

static void tc_dedup_basic() {
    begin_test("TC-04: msg_id dedup — 기본 동작");

    std::unordered_set<std::string> seen;

    // 첫 삽입: 새로운 msg_id
    std::string id1 = MSG_EVT;
    CHECK_TRUE(seen.count(id1) == 0);
    seen.insert(id1);
    CHECK_EQ((int)seen.size(), 1);

    // 두 번째: 중복
    CHECK_TRUE(seen.count(id1) != 0);

    // 다른 id는 통과
    std::string id2 = "ffffffff-ffff-4fff-bfff-ffffffffffff";
    CHECK_TRUE(seen.count(id2) == 0);
    seen.insert(id2);
    CHECK_EQ((int)seen.size(), 2);

    end_test("TC-04: msg_id dedup — 기본 동작");
}

// ── TC-05: msg_id seen_set 용량 제한 ─────────────────────────────────────────

static void tc_dedup_cap() {
    begin_test("TC-05: seen_msg_ids cap — 10001개 삽입 후 clear");

    std::unordered_set<std::string> seen;
    char buf[64];

    for (int i = 0; i <= 10000; i++) {
        snprintf(buf, sizeof(buf), "msg-%05d", i);
        seen.insert(std::string(buf));
    }
    CHECK_EQ((int)seen.size(), 10001);

    // 용량 초과 시 clear (core/main.cpp 로직과 동일)
    if (seen.size() > 10000) seen.clear();
    CHECK_EQ((int)seen.size(), 0);

    // clear 후 재삽입 정상 동작 확인
    seen.insert("msg-new");
    CHECK_EQ((int)seen.size(), 1);

    end_test("TC-05: seen_msg_ids cap — 10001개 삽입 후 clear");
}

// ── TC-06: Edge 등록 메시지 round-trip ───────────────────────────────────────
// Edge on_connect_core 가 발행하는 STATUS JSON 을 파싱해 description → ip:port 추출

static void tc_edge_registration_roundtrip() {
    begin_test("TC-06: Edge 등록 STATUS JSON round-trip");

    // Edge 가 실제로 publish 하는 것과 동일한 형식의 JSON
    const char* status_json =
        "{"
        "\"msg_id\":\"" MSG_EVT "\","
        "\"type\":\"STATUS\","
        "\"timestamp\":\"2026-04-16T00:00:00Z\","
        "\"source\":{\"role\":\"NODE\",\"id\":\"" NODE_1 "\"},"
        "\"target\":{\"role\":\"CORE\",\"id\":\"\"},"
        "\"route\":{\"original_node\":\"\",\"prev_hop\":\"\",\"next_hop\":\"\","
        "           \"hop_count\":0,\"ttl\":0},"
        "\"delivery\":{\"qos\":1,\"dup\":false,\"retain\":false},"
        "\"payload\":{\"building_id\":\"\",\"camera_id\":\"\","
        "             \"description\":\"10.0.0.5:1884\"}"
        "}";

    MqttMessage reg = {};
    CHECK_TRUE(mqtt_message_from_json(std::string(status_json), reg));
    CHECK_EQ(reg.type, MSG_TYPE_STATUS);
    CHECK_STREQ(reg.source.id, NODE_1);

    // description 에서 ip:port 파싱
    char ip[64] = {};
    int  port = 0;
    CHECK_TRUE(parse_ip_port(reg.payload.description, ip, sizeof(ip), &port));
    CHECK_STREQ(ip, "10.0.0.5");
    CHECK_EQ(port, 1884);

    end_test("TC-06: Edge 등록 STATUS JSON round-trip");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    tc_parse_ip_port_normal();
    tc_parse_ip_port_invalid();
    tc_make_alert_topic();
    tc_dedup_basic();
    tc_dedup_cap();
    tc_edge_registration_roundtrip();

    printf("══════════════════════════════════════\n");
    printf("  결과: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════\n");
    return g_fail == 0 ? 0 : 1;
}
