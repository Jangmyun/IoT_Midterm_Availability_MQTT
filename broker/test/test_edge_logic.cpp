// test_edge_logic.cpp
// Edge 헬퍼 함수 단위 테스트: infer_msg_type, infer_priority,
//                              parse_building_camera, select_relay_node,
//                              upstream_find, upstream_preferred, upstream_choose
//
// 외부 프레임워크 없음 — 빌드 후 ./build/test_edge_logic 으로 실행

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include "edge_helpers.h"
#include "edge_upstream.h"
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "publisher_helpers.h"

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

#define SELF_ID   "aaaaaaaa-0000-0000-0000-000000000001"
#define NODE_A    "bbbbbbbb-0000-0000-0000-000000000002"
#define NODE_B    "cccccccc-0000-0000-0000-000000000003"
#define NODE_C    "dddddddd-0000-0000-0000-000000000004"
#define CORE_A    "eeeeeeee-0000-0000-0000-000000000005"
#define CORE_B    "ffffffff-0000-0000-0000-000000000006"

// ── TC-01: infer_msg_type ─────────────────────────────────────────────────────

static void tc_infer_msg_type() {
    begin_test("TC-01: infer_msg_type — 토픽/페이로드 기반 타입 추론");

    // 토픽에 intrusion 포함 → INTRUSION
    CHECK_EQ(infer_msg_type("campus/data/INTRUSION", ""), MSG_TYPE_INTRUSION);

    // 페이로드에 intrusion 포함 (대소문자 무관) → INTRUSION
    CHECK_EQ(infer_msg_type("campus/data/event", "Intrusion detected"), MSG_TYPE_INTRUSION);

    // 토픽에 door 포함 → DOOR_FORCED
    CHECK_EQ(infer_msg_type("campus/data/DOOR_FORCED", ""), MSG_TYPE_DOOR_FORCED);

    // 페이로드에 door 포함 → DOOR_FORCED
    CHECK_EQ(infer_msg_type("campus/data/event", "door forced open"), MSG_TYPE_DOOR_FORCED);

    // intrusion 없고 door 없으면 → MOTION
    CHECK_EQ(infer_msg_type("campus/data/MOTION", ""), MSG_TYPE_MOTION);
    CHECK_EQ(infer_msg_type("campus/data/event", "object detected"), MSG_TYPE_MOTION);

    // 빈 토픽/페이로드 → MOTION (기본값)
    CHECK_EQ(infer_msg_type("", ""), MSG_TYPE_MOTION);
    CHECK_EQ(infer_msg_type(nullptr, ""), MSG_TYPE_MOTION);

    // intrusion이 door보다 우선 (토픽에 둘 다 포함되는 경우는 없지만 함수 순서 확인)
    CHECK_EQ(infer_msg_type("campus/data/INTRUSION", "door"), MSG_TYPE_INTRUSION);

    end_test("TC-01: infer_msg_type — 토픽/페이로드 기반 타입 추론");
}

// ── TC-02: infer_priority ─────────────────────────────────────────────────────

static void tc_infer_priority() {
    begin_test("TC-02: infer_priority — MsgType별 우선순위 매핑");

    CHECK_EQ(infer_priority(MSG_TYPE_INTRUSION),   PRIORITY_HIGH);
    CHECK_EQ(infer_priority(MSG_TYPE_DOOR_FORCED), PRIORITY_HIGH);
    CHECK_EQ(infer_priority(MSG_TYPE_MOTION),      PRIORITY_MEDIUM);

    // 나머지 타입은 PRIORITY_NONE
    CHECK_EQ(infer_priority(MSG_TYPE_STATUS),        PRIORITY_NONE);
    CHECK_EQ(infer_priority(MSG_TYPE_LWT_CORE),      PRIORITY_NONE);
    CHECK_EQ(infer_priority(MSG_TYPE_LWT_NODE),      PRIORITY_NONE);
    CHECK_EQ(infer_priority(MSG_TYPE_PING_REQUEST),  PRIORITY_NONE);
    CHECK_EQ(infer_priority(MSG_TYPE_PING_RESPONSE), PRIORITY_NONE);

    end_test("TC-02: infer_priority — MsgType별 우선순위 매핑");
}

// ── TC-03: parse_building_camera ──────────────────────────────────────────────

static void tc_parse_building_camera() {
    begin_test("TC-03: parse_building_camera — 토픽에서 building/camera 추출");

    char bld[64], cam[64];

    // campus/data/<event>/<building>/<camera> → building + camera 분리
    parse_building_camera("campus/data/INTRUSION/building-a/cam-01",
        bld, sizeof(bld), cam, sizeof(cam));
    CHECK_STREQ(bld, "INTRUSION");
    CHECK_STREQ(cam, "building-a/cam-01");

    // campus/data/<event> 만 있을 때 → building에 event, camera 빈 문자열
    memset(bld, 0, sizeof(bld));
    memset(cam, 0, sizeof(cam));
    parse_building_camera("campus/data/MOTION",
        bld, sizeof(bld), cam, sizeof(cam));
    CHECK_STREQ(bld, "MOTION");
    CHECK_STREQ(cam, "");

    // prefix 불일치 → 모두 빈 문자열
    memset(bld, 0, sizeof(bld));
    memset(cam, 0, sizeof(cam));
    parse_building_camera("unrelated/topic/data",
        bld, sizeof(bld), cam, sizeof(cam));
    CHECK_STREQ(bld, "");
    CHECK_STREQ(cam, "");

    // nullptr 토픽 → 모두 빈 문자열 (crash 없이)
    memset(bld, 0, sizeof(bld));
    memset(cam, 0, sizeof(cam));
    parse_building_camera(nullptr, bld, sizeof(bld), cam, sizeof(cam));
    CHECK_STREQ(bld, "");
    CHECK_STREQ(cam, "");

    end_test("TC-03: parse_building_camera — 토픽에서 building/camera 추출");
}

// ── TC-03A: extract_nested_event_origin_node ─────────────────────────────────

static void tc_extract_nested_event_origin_node() {
    begin_test("TC-03A: extract_nested_event_origin_node — nested publisher route.original_node 추출");

    MqttMessage nested = {};
    CHECK_TRUE(build_event_message(SELF_ID, MSG_TYPE_INTRUSION,
                                   "building-a", "cam-01", "sim", 1, &nested));

    std::strncpy(nested.route.original_node, NODE_B, UUID_LEN - 1);
    nested.route.original_node[UUID_LEN - 1] = '\0';
    std::string json = mqtt_message_to_json(nested);

    char out[UUID_LEN] = {};
    CHECK_TRUE(extract_nested_event_origin_node(json, out, sizeof(out)));
    CHECK_STREQ(out, NODE_B);

    std::strncpy(nested.route.original_node, nested.source.id, UUID_LEN - 1);
    nested.route.original_node[UUID_LEN - 1] = '\0';
    json = mqtt_message_to_json(nested);

    std::memset(out, 0, sizeof(out));
    CHECK_FALSE(extract_nested_event_origin_node(json, out, sizeof(out)));
    CHECK_FALSE(extract_nested_event_origin_node("{bad-json", out, sizeof(out)));

    end_test("TC-03A: extract_nested_event_origin_node — nested publisher route.original_node 추출");
}

// ── TC-04: select_relay_node — RTT 최소 노드 선택 ────────────────────────────

static void tc_select_relay_rtt_min() {
    begin_test("TC-04: select_relay_node — RTT 최소 노드 선택");

    ConnectionTableManager ct;
    ct.init("", "");

    // SELF 노드 등록 (자신 — 후보 제외 확인용)
    NodeEntry self = {};
    strncpy(self.id, SELF_ID, UUID_LEN - 1);
    self.role = NODE_ROLE_NODE; self.status = NODE_STATUS_ONLINE; self.hop_to_core = 1;
    ct.addNode(self);

    // NODE_A: RTT 5ms
    NodeEntry na = {};
    strncpy(na.id, NODE_A, UUID_LEN - 1);
    na.role = NODE_ROLE_NODE; na.status = NODE_STATUS_ONLINE; na.hop_to_core = 2;
    ct.addNode(na);

    LinkEntry la = {};
    strncpy(la.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(la.to_id,   NODE_A,  UUID_LEN - 1);
    la.rtt_ms = 5.0f;
    ct.addLink(la);

    // NODE_B: RTT 20ms
    NodeEntry nb = {};
    strncpy(nb.id, NODE_B, UUID_LEN - 1);
    nb.role = NODE_ROLE_NODE; nb.status = NODE_STATUS_ONLINE; nb.hop_to_core = 1;
    ct.addNode(nb);

    LinkEntry lb = {};
    strncpy(lb.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(lb.to_id,   NODE_B,  UUID_LEN - 1);
    lb.rtt_ms = 20.0f;
    ct.addLink(lb);

    std::string best = select_relay_node(ct, SELF_ID);
    CHECK_STREQ(best.c_str(), NODE_A);  // RTT 5 < 20 → NODE_A

    end_test("TC-04: select_relay_node — RTT 최소 노드 선택");
}

// ── TC-05: select_relay_node — RTT 동점 시 hop_to_core 최소 선택 ─────────────

static void tc_select_relay_hop_tiebreak() {
    begin_test("TC-05: select_relay_node — RTT 동점 시 hop_to_core 최소");

    ConnectionTableManager ct;
    ct.init("", "");

    NodeEntry self = {};
    strncpy(self.id, SELF_ID, UUID_LEN - 1);
    self.role = NODE_ROLE_NODE; self.status = NODE_STATUS_ONLINE; self.hop_to_core = 1;
    ct.addNode(self);

    // NODE_A: RTT 10ms, hop 3
    NodeEntry na = {};
    strncpy(na.id, NODE_A, UUID_LEN - 1);
    na.role = NODE_ROLE_NODE; na.status = NODE_STATUS_ONLINE; na.hop_to_core = 3;
    ct.addNode(na);

    LinkEntry la = {};
    strncpy(la.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(la.to_id,   NODE_A,  UUID_LEN - 1);
    la.rtt_ms = 10.0f;
    ct.addLink(la);

    // NODE_B: RTT 10ms, hop 1 (동 RTT, 더 가까운 hop)
    NodeEntry nb = {};
    strncpy(nb.id, NODE_B, UUID_LEN - 1);
    nb.role = NODE_ROLE_NODE; nb.status = NODE_STATUS_ONLINE; nb.hop_to_core = 1;
    ct.addNode(nb);

    LinkEntry lb = {};
    strncpy(lb.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(lb.to_id,   NODE_B,  UUID_LEN - 1);
    lb.rtt_ms = 10.0f;
    ct.addLink(lb);

    std::string best = select_relay_node(ct, SELF_ID);
    CHECK_STREQ(best.c_str(), NODE_B);  // RTT 동점 → hop 1 < 3 → NODE_B

    end_test("TC-05: select_relay_node — RTT 동점 시 hop_to_core 최소");
}

// ── TC-06: select_relay_node — OFFLINE 노드 제외 ─────────────────────────────

static void tc_select_relay_offline_excluded() {
    begin_test("TC-06: select_relay_node — OFFLINE 노드 제외");

    ConnectionTableManager ct;
    ct.init("", "");

    NodeEntry self = {};
    strncpy(self.id, SELF_ID, UUID_LEN - 1);
    self.role = NODE_ROLE_NODE; self.status = NODE_STATUS_ONLINE; self.hop_to_core = 1;
    ct.addNode(self);

    // NODE_A: OFFLINE (RTT 있어도 후보 제외)
    NodeEntry na = {};
    strncpy(na.id, NODE_A, UUID_LEN - 1);
    na.role = NODE_ROLE_NODE; na.status = NODE_STATUS_OFFLINE; na.hop_to_core = 1;
    ct.addNode(na);

    LinkEntry la = {};
    strncpy(la.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(la.to_id,   NODE_A,  UUID_LEN - 1);
    la.rtt_ms = 3.0f;
    ct.addLink(la);

    // NODE_B: ONLINE, RTT 15ms
    NodeEntry nb = {};
    strncpy(nb.id, NODE_B, UUID_LEN - 1);
    nb.role = NODE_ROLE_NODE; nb.status = NODE_STATUS_ONLINE; nb.hop_to_core = 2;
    ct.addNode(nb);

    LinkEntry lb = {};
    strncpy(lb.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(lb.to_id,   NODE_B,  UUID_LEN - 1);
    lb.rtt_ms = 15.0f;
    ct.addLink(lb);

    std::string best = select_relay_node(ct, SELF_ID);
    CHECK_STREQ(best.c_str(), NODE_B);  // NODE_A는 OFFLINE → NODE_B 선택

    end_test("TC-06: select_relay_node — OFFLINE 노드 제외");
}

// ── TC-07: select_relay_node — RTT 미측정 노드 제외 ─────────────────────────

static void tc_select_relay_no_rtt_excluded() {
    begin_test("TC-07: select_relay_node — RTT 미측정 노드 제외");

    ConnectionTableManager ct;
    ct.init("", "");

    NodeEntry self = {};
    strncpy(self.id, SELF_ID, UUID_LEN - 1);
    self.role = NODE_ROLE_NODE; self.status = NODE_STATUS_ONLINE; self.hop_to_core = 1;
    ct.addNode(self);

    // NODE_A: ONLINE 이지만 링크(RTT) 없음 → 후보 제외
    NodeEntry na = {};
    strncpy(na.id, NODE_A, UUID_LEN - 1);
    na.role = NODE_ROLE_NODE; na.status = NODE_STATUS_ONLINE; na.hop_to_core = 1;
    ct.addNode(na);
    // 링크 없음

    // NODE_B: ONLINE, RTT 있음
    NodeEntry nb = {};
    strncpy(nb.id, NODE_B, UUID_LEN - 1);
    nb.role = NODE_ROLE_NODE; nb.status = NODE_STATUS_ONLINE; nb.hop_to_core = 2;
    ct.addNode(nb);

    LinkEntry lb = {};
    strncpy(lb.from_id, SELF_ID, UUID_LEN - 1);
    strncpy(lb.to_id,   NODE_B,  UUID_LEN - 1);
    lb.rtt_ms = 8.0f;
    ct.addLink(lb);

    std::string best = select_relay_node(ct, SELF_ID);
    CHECK_STREQ(best.c_str(), NODE_B);  // NODE_A RTT 없음 → NODE_B

    end_test("TC-07: select_relay_node — RTT 미측정 노드 제외");
}

// ── TC-08: select_relay_node — 후보 없을 때 빈 문자열 ───────────────────────

static void tc_select_relay_no_candidates() {
    begin_test("TC-08: select_relay_node — 후보 없을 때 빈 문자열 반환");

    // 케이스 A: CT 비어 있음
    {
        ConnectionTableManager ct;
        ct.init("", "");
        std::string best = select_relay_node(ct, SELF_ID);
        CHECK_TRUE(best.empty());
    }

    // 케이스 B: CORE 역할만 있고 NODE 역할 없음
    {
        ConnectionTableManager ct;
        ct.init("", "");
        NodeEntry core = {};
        strncpy(core.id, NODE_A, UUID_LEN - 1);
        core.role = NODE_ROLE_CORE; core.status = NODE_STATUS_ONLINE; core.hop_to_core = 0;
        ct.addNode(core);

        LinkEntry lc = {};
        strncpy(lc.from_id, SELF_ID, UUID_LEN - 1);
        strncpy(lc.to_id,   NODE_A,  UUID_LEN - 1);
        lc.rtt_ms = 1.0f;
        ct.addLink(lc);

        std::string best = select_relay_node(ct, SELF_ID);
        CHECK_TRUE(best.empty());  // CORE는 Relay 후보 아님
    }

    // 케이스 C: NODE 있지만 모두 OFFLINE
    {
        ConnectionTableManager ct;
        ct.init("", "");
        NodeEntry na = {};
        strncpy(na.id, NODE_A, UUID_LEN - 1);
        na.role = NODE_ROLE_NODE; na.status = NODE_STATUS_OFFLINE; na.hop_to_core = 1;
        ct.addNode(na);

        LinkEntry la = {};
        strncpy(la.from_id, SELF_ID, UUID_LEN - 1);
        strncpy(la.to_id,   NODE_A,  UUID_LEN - 1);
        la.rtt_ms = 5.0f;
        ct.addLink(la);

        std::string best = select_relay_node(ct, SELF_ID);
        CHECK_TRUE(best.empty());
    }

    end_test("TC-08: select_relay_node — 후보 없을 때 빈 문자열 반환");
}

// ── TC-11~17: upstream_find / upstream_preferred / upstream_choose ────────────

// 테스트용 더미 non-null mosquitto 포인터 (실제 연결 불필요)
static struct mosquitto* DUMMY_MOSQ = reinterpret_cast<struct mosquitto*>(0x1);

static void tc_upstream_find_returns_connected_core() {
    begin_test("TC-11: upstream_find — connected CORE 반환");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = true;
    conns[1].mosq      = DUMMY_MOSQ;
    conns[1].kind      = UpstreamKind::BACKUP;
    conns[1].connected = true;

    UpstreamConn* result = upstream_find(conns, 2, UpstreamKind::CORE);
    CHECK(result != nullptr);
    CHECK(result == &conns[0]);

    end_test("TC-11: upstream_find — connected CORE 반환");
}

static void tc_upstream_find_skips_disconnected() {
    begin_test("TC-12: upstream_find — disconnected 슬롯 건너뜀");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = false;  // disconnected

    UpstreamConn* result = upstream_find(conns, 1, UpstreamKind::CORE);
    CHECK(result == nullptr);

    end_test("TC-12: upstream_find — disconnected 슬롯 건너뜀");
}

static void tc_upstream_find_skips_tombstone() {
    begin_test("TC-13: upstream_find — mosq=nullptr tombstone 건너뜀");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = nullptr;  // tombstone
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = true;

    UpstreamConn* result = upstream_find(conns, 1, UpstreamKind::CORE);
    CHECK(result == nullptr);

    end_test("TC-13: upstream_find — mosq=nullptr tombstone 건너뜀");
}

static void tc_upstream_preferred_returns_flagged_slot() {
    begin_test("TC-14: upstream_preferred — preferred=true 슬롯 반환");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = true;
    conns[0].preferred = false;

    conns[1].mosq      = DUMMY_MOSQ;
    conns[1].kind      = UpstreamKind::BACKUP;
    conns[1].connected = true;
    conns[1].preferred = true;  // ← preferred

    UpstreamConn* result = upstream_preferred(conns, 2);
    CHECK(result != nullptr);
    CHECK(result == &conns[1]);

    end_test("TC-14: upstream_preferred — preferred=true 슬롯 반환");
}

static void tc_upstream_choose_prefers_preferred_over_core() {
    begin_test("TC-15: upstream_choose — preferred 슬롯이 CORE 보다 우선");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = true;
    conns[0].preferred = false;

    conns[1].mosq      = DUMMY_MOSQ;
    conns[1].kind      = UpstreamKind::BACKUP;
    conns[1].connected = true;
    conns[1].preferred = true;  // ← preferred

    UpstreamConn* chosen = upstream_choose(conns, 2);
    CHECK(chosen != nullptr);
    CHECK(chosen == &conns[1]);  // preferred BACKUP 선택됨

    end_test("TC-15: upstream_choose — preferred 슬롯이 CORE 보다 우선");
}

static void tc_upstream_choose_fallback_order() {
    begin_test("TC-16: upstream_choose — CORE 불가 시 BACKUP → PEER_EDGE 순 폴백");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    // CORE: disconnected
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = false;
    // BACKUP: connected
    conns[1].mosq      = DUMMY_MOSQ;
    conns[1].kind      = UpstreamKind::BACKUP;
    conns[1].connected = true;
    // PEER_EDGE: connected
    conns[2].mosq      = DUMMY_MOSQ;
    conns[2].kind      = UpstreamKind::PEER_EDGE;
    conns[2].connected = true;

    // CORE 불가 → BACKUP 선택
    UpstreamConn* chosen = upstream_choose(conns, 3);
    CHECK(chosen != nullptr);
    CHECK(chosen->kind == UpstreamKind::BACKUP);

    // BACKUP 도 끊으면 → PEER_EDGE 선택
    conns[1].connected = false;
    chosen = upstream_choose(conns, 3);
    CHECK(chosen != nullptr);
    CHECK(chosen->kind == UpstreamKind::PEER_EDGE);

    end_test("TC-16: upstream_choose — CORE 불가 시 BACKUP → PEER_EDGE 순 폴백");
}

static void tc_upstream_choose_returns_null_when_all_disconnected() {
    begin_test("TC-17: upstream_choose — 모든 슬롯 disconnected 시 nullptr");

    UpstreamConn conns[MAX_UPSTREAM] = {};
    conns[0].mosq      = DUMMY_MOSQ;
    conns[0].kind      = UpstreamKind::CORE;
    conns[0].connected = false;
    conns[1].mosq      = DUMMY_MOSQ;
    conns[1].kind      = UpstreamKind::BACKUP;
    conns[1].connected = false;
    conns[2].mosq      = DUMMY_MOSQ;
    conns[2].kind      = UpstreamKind::PEER_EDGE;
    conns[2].connected = false;

    UpstreamConn* chosen = upstream_choose(conns, 3);
    CHECK(chosen == nullptr);

    end_test("TC-17: upstream_choose — 모든 슬롯 disconnected 시 nullptr");
}

// ── TC-09: should_failover_on_core_will — active core에만 반응 ───────────────

static void tc_should_failover_on_active_core_will() {
    begin_test("TC-09: should_failover_on_core_will — active core에만 반응");

    ConnectionTableManager ct;
    ct.init(CORE_A, CORE_B);

    CHECK_TRUE(should_failover_on_core_will(ct, CORE_A));
    CHECK_FALSE(should_failover_on_core_will(ct, CORE_B));
    CHECK_TRUE(is_backup_core_will(ct, CORE_B));
    CHECK_FALSE(is_backup_core_will(ct, CORE_A));

    end_test("TC-09: should_failover_on_core_will — active core에만 반응");
}

// ── TC-10: should_failover_on_core_will — CT 미동기화 시 보수적 true ─────────

static void tc_should_failover_without_active_core_id() {
    begin_test("TC-10: should_failover_on_core_will — CT 미동기화 시 보수적 true");

    ConnectionTableManager ct;
    ct.init("", CORE_B);

    CHECK_TRUE(should_failover_on_core_will(ct, CORE_A));
    CHECK_FALSE(is_backup_core_will(ct, CORE_A));
    CHECK_TRUE(is_backup_core_will(ct, CORE_B));

    end_test("TC-10: should_failover_on_core_will — CT 미동기화 시 보수적 true");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("════════════════════════════════════════════════════\n");
    printf(" Edge Logic Tests\n");
    printf("════════════════════════════════════════════════════\n\n");

    tc_infer_msg_type();
    tc_infer_priority();
    tc_parse_building_camera();
    tc_extract_nested_event_origin_node();
    tc_select_relay_rtt_min();
    tc_select_relay_hop_tiebreak();
    tc_select_relay_offline_excluded();
    tc_select_relay_no_rtt_excluded();
    tc_select_relay_no_candidates();
    tc_should_failover_on_active_core_will();
    tc_should_failover_without_active_core_id();
    tc_upstream_find_returns_connected_core();
    tc_upstream_find_skips_disconnected();
    tc_upstream_find_skips_tombstone();
    tc_upstream_preferred_returns_flagged_slot();
    tc_upstream_choose_prefers_preferred_over_core();
    tc_upstream_choose_fallback_order();
    tc_upstream_choose_returns_null_when_all_disconnected();

    printf("══════════════════════════════════════\n");
    printf("  결과: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════\n");
    return g_fail == 0 ? 0 : 1;
}
