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
#include "connection_table_manager.h"

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
#define CORE_A  "aaaaaaaa-0000-0000-0000-000000000001"
#define CORE_B  "bbbbbbbb-0000-0000-0000-000000000002"
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

// ── TC-07: merge_connection_tables — 비겹침 노드 ──────────────────────────────

static void tc_merge_disjoint() {
    begin_test("TC-07: merge_connection_tables — 비겹침 노드");

    // Local CT: node-1, node-2
    ConnectionTableManager local;
    local.init("", "");
    {
        NodeEntry n1 = {};
        strncpy(n1.id, NODE_1, UUID_LEN - 1);
        n1.role = NODE_ROLE_NODE;
        strncpy(n1.ip, "10.0.0.1", IP_LEN - 1);
        n1.port = 1883; n1.status = NODE_STATUS_ONLINE; n1.hop_to_core = 1;
        local.addNode(n1);
    }
    {
        NodeEntry n2 = {};
        strncpy(n2.id, "dddddddd-0000-0000-0000-000000000004", UUID_LEN - 1);
        n2.role = NODE_ROLE_NODE;
        strncpy(n2.ip, "10.0.0.2", IP_LEN - 1);
        n2.port = 1883; n2.status = NODE_STATUS_ONLINE; n2.hop_to_core = 1;
        local.addNode(n2);
    }

    // Remote CT: node-2 (same), node-3 (new)
    ConnectionTable remote = {};
    {
        NodeEntry n2 = {};
        strncpy(n2.id, "dddddddd-0000-0000-0000-000000000004", UUID_LEN - 1);
        n2.role = NODE_ROLE_NODE; n2.status = NODE_STATUS_ONLINE; n2.hop_to_core = 1;
        remote.nodes[remote.node_count++] = n2;
    }
    {
        NodeEntry n3 = {};
        strncpy(n3.id, "eeeeeeee-0000-0000-0000-000000000005", UUID_LEN - 1);
        n3.role = NODE_ROLE_NODE;
        strncpy(n3.ip, "10.0.0.3", IP_LEN - 1);
        n3.port = 1883; n3.status = NODE_STATUS_ONLINE; n3.hop_to_core = 1;
        remote.nodes[remote.node_count++] = n3;
    }

    bool changed = merge_connection_tables(local, remote);
    CHECK_TRUE(changed);

    ConnectionTable result = local.snapshot();
    CHECK_EQ(result.node_count, 3);  // node-1, node-2, node-3
    CHECK_TRUE(local.findNode("eeeeeeee-0000-0000-0000-000000000005").has_value());
    CHECK_TRUE(local.findNode(NODE_1).has_value());

    end_test("TC-07: merge_connection_tables — 비겹침 노드");
}

// ── TC-08: merge_connection_tables — 최신 snapshot 반영 ──────────────────────

static void tc_merge_latest_snapshot_wins() {
    begin_test("TC-08: merge_connection_tables — 최신 snapshot 반영");

    // Local: node-1 OFFLINE
    ConnectionTableManager local;
    local.init("", "");
    {
        NodeEntry n = {};
        strncpy(n.id, NODE_1, UUID_LEN - 1);
        n.role = NODE_ROLE_NODE;
        strncpy(n.ip, "10.0.0.1", IP_LEN - 1);
        n.port = 1883; n.status = NODE_STATUS_OFFLINE; n.hop_to_core = 1;
        local.addNode(n);
    }

    // Remote: node-1 ONLINE
    ConnectionTable remote = {};
    {
        NodeEntry n = {};
        strncpy(n.id, NODE_1, UUID_LEN - 1);
        n.role = NODE_ROLE_NODE;
        strncpy(n.ip, "10.0.0.1", IP_LEN - 1);
        n.port = 1883; n.status = NODE_STATUS_ONLINE; n.hop_to_core = 1;
        remote.nodes[remote.node_count++] = n;
    }

    bool changed = merge_connection_tables(local, remote);
    CHECK_TRUE(changed);

    auto result = local.findNode(NODE_1);
    CHECK_TRUE(result.has_value());
    CHECK_EQ(result->status, NODE_STATUS_ONLINE);

    // 역방향: Remote OFFLINE, Local ONLINE → remote snapshot을 반영
    ConnectionTable remote2 = {};
    {
        NodeEntry n = {};
        strncpy(n.id, NODE_1, UUID_LEN - 1);
        n.role = NODE_ROLE_NODE;
        strncpy(n.ip, "10.0.0.1", IP_LEN - 1);
        n.port = 1883;
        n.status = NODE_STATUS_OFFLINE;
        n.hop_to_core = 1;
        remote2.nodes[remote2.node_count++] = n;
    }
    bool changed2 = merge_connection_tables(local, remote2);
    CHECK_TRUE(changed2);

    auto result2 = local.findNode(NODE_1);
    CHECK_TRUE(result2.has_value());
    CHECK_EQ(result2->status, NODE_STATUS_OFFLINE);

    end_test("TC-08: merge_connection_tables — 최신 snapshot 반영");
}

// ── TC-09: addLink — identical input is idempotent ──────────────────────────

static void tc_add_link_idempotent() {
    begin_test("TC-09: addLink — identical input is idempotent");

    ConnectionTableManager ct;
    ct.init("", "");

    LinkEntry link = {};
    std::strncpy(link.from_id, CORE_A, UUID_LEN - 1);
    std::strncpy(link.to_id, CORE_B, UUID_LEN - 1);
    link.rtt_ms = 0.0f;

    CHECK_TRUE(ct.addLink(link));
    int version_after_first_add = ct.snapshot().version;

    CHECK_FALSE(ct.addLink(link));
    CHECK_EQ(ct.snapshot().version, version_after_first_add);

    link.rtt_ms = 1.25f;
    CHECK_TRUE(ct.addLink(link));
    CHECK_EQ(ct.snapshot().version, version_after_first_add + 1);

    end_test("TC-09: addLink — identical input is idempotent");
}

// ── TC-10: merge_backup_registration — equal version bootstrap merge ────────

static void tc_merge_backup_registration_equal_version() {
    begin_test("TC-10: merge_backup_registration — equal version bootstrap merge");

    ConnectionTableManager local;
    local.init(CORE_A, "");
    {
        NodeEntry active = {};
        std::strncpy(active.id, CORE_A, UUID_LEN - 1);
        active.role = NODE_ROLE_CORE;
        std::strncpy(active.ip, "127.0.0.1", IP_LEN - 1);
        active.port = 1883;
        active.status = NODE_STATUS_ONLINE;
        active.hop_to_core = 0;
        local.addNode(active);
    }

    ConnectionTable remote = {};
    remote.version = 1;
    std::strncpy(remote.backup_core_id, CORE_B, UUID_LEN - 1);
    {
        NodeEntry backup = {};
        std::strncpy(backup.id, CORE_B, UUID_LEN - 1);
        backup.role = NODE_ROLE_CORE;
        std::strncpy(backup.ip, "127.0.0.1", IP_LEN - 1);
        backup.port = 1884;
        backup.status = NODE_STATUS_ONLINE;
        backup.hop_to_core = 1;
        remote.nodes[remote.node_count++] = backup;
    }

    CHECK_TRUE(merge_backup_registration(local, CORE_A, remote));

    ConnectionTable first = local.snapshot();
    CHECK_STREQ(first.active_core_id, CORE_A);
    CHECK_STREQ(first.backup_core_id, CORE_B);
    CHECK_EQ(first.node_count, 2);
    CHECK_TRUE(local.findNode(CORE_B).has_value());
    CHECK_TRUE(local.findLink(CORE_A, CORE_B).has_value());

    CHECK_FALSE(merge_backup_registration(local, CORE_A, remote));

    end_test("TC-10: merge_backup_registration — equal version bootstrap merge");
}

// ── TC-11: merge_backup_registration — stale backup이 online edge를 못 내림 ─

static void tc_merge_backup_registration_preserves_active_online_edges() {
    begin_test("TC-11: merge_backup_registration — stale backup이 online edge를 못 내림");

    ConnectionTableManager local;
    local.init(CORE_A, CORE_B);

    {
        NodeEntry active = {};
        std::strncpy(active.id, CORE_A, UUID_LEN - 1);
        active.role = NODE_ROLE_CORE;
        std::strncpy(active.ip, "192.168.0.7", IP_LEN - 1);
        active.port = 1883;
        active.status = NODE_STATUS_ONLINE;
        active.hop_to_core = 0;
        CHECK_TRUE(local.addNode(active));
    }

    {
        NodeEntry edge = {};
        std::strncpy(edge.id, NODE_1, UUID_LEN - 1);
        edge.role = NODE_ROLE_NODE;
        std::strncpy(edge.ip, "192.168.0.9", IP_LEN - 1);
        edge.port = 1883;
        edge.status = NODE_STATUS_ONLINE;
        edge.hop_to_core = 1;
        CHECK_TRUE(local.addNode(edge));
    }

    ConnectionTable remote = {};
    remote.version = 9;
    std::strncpy(remote.backup_core_id, CORE_B, UUID_LEN - 1);
    {
        NodeEntry backup = {};
        std::strncpy(backup.id, CORE_B, UUID_LEN - 1);
        backup.role = NODE_ROLE_CORE;
        std::strncpy(backup.ip, "192.168.0.8", IP_LEN - 1);
        backup.port = 1883;
        backup.status = NODE_STATUS_ONLINE;
        backup.hop_to_core = 1;
        remote.nodes[remote.node_count++] = backup;
    }
    {
        NodeEntry edge = {};
        std::strncpy(edge.id, NODE_1, UUID_LEN - 1);
        edge.role = NODE_ROLE_NODE;
        std::strncpy(edge.ip, "192.168.0.9", IP_LEN - 1);
        edge.port = 1883;
        edge.status = NODE_STATUS_OFFLINE;
        edge.hop_to_core = 1;
        remote.nodes[remote.node_count++] = edge;
    }

    CHECK_TRUE(merge_backup_registration(local, CORE_A, remote));

    auto mergedEdge = local.findNode(NODE_1);
    CHECK_TRUE(mergedEdge.has_value());
    CHECK_EQ(mergedEdge->status, NODE_STATUS_ONLINE);

    end_test("TC-11: merge_backup_registration — stale backup이 online edge를 못 내림");
}

// ── TC-12: promote_core_after_failover — 이전 active OFFLINE 처리 ───────────

static void tc_promote_core_after_failover_marks_failed_active_offline() {
    begin_test("TC-12: promote_core_after_failover — 이전 active OFFLINE 처리");

    ConnectionTableManager ct;
    ct.init(CORE_A, CORE_B);

    NodeEntry active = {};
    std::strncpy(active.id, CORE_A, UUID_LEN - 1);
    active.role = NODE_ROLE_CORE;
    std::strncpy(active.ip, "192.168.0.7", IP_LEN - 1);
    active.port = 1883;
    active.status = NODE_STATUS_ONLINE;
    active.hop_to_core = 0;
    CHECK_TRUE(ct.addNode(active));

    NodeEntry backup = {};
    std::strncpy(backup.id, CORE_B, UUID_LEN - 1);
    backup.role = NODE_ROLE_CORE;
    std::strncpy(backup.ip, "192.168.0.16", IP_LEN - 1);
    backup.port = 1883;
    backup.status = NODE_STATUS_ONLINE;
    backup.hop_to_core = 1;
    CHECK_TRUE(ct.addNode(backup));

    CHECK_TRUE(promote_core_after_failover(ct, CORE_B, CORE_A));

    ConnectionTable snapshot = ct.snapshot();
    CHECK_STREQ(snapshot.active_core_id, CORE_B);
    CHECK_STREQ(snapshot.backup_core_id, "");
    CHECK_EQ(snapshot.node_count, 2);

    auto failedActive = ct.findNode(CORE_A);
    auto promotedCore = ct.findNode(CORE_B);
    CHECK_TRUE(failedActive.has_value());
    CHECK_TRUE(promotedCore.has_value());
    CHECK_EQ(failedActive->status, NODE_STATUS_OFFLINE);
    CHECK_EQ(promotedCore->status, NODE_STATUS_ONLINE);

    end_test("TC-12: promote_core_after_failover — 이전 active OFFLINE 처리");
}

// ── TC-13: should_promote_backup_on_core_will — active에만 반응 ───────────────

static void tc_should_promote_backup_on_active_only() {
    begin_test("TC-13: should_promote_backup_on_core_will — active에만 반응");

    ConnectionTableManager ct;
    ct.init(CORE_A, CORE_B);

    NodeEntry active = {};
    std::strncpy(active.id, CORE_A, UUID_LEN - 1);
    active.role = NODE_ROLE_CORE;
    active.status = NODE_STATUS_ONLINE;
    CHECK_TRUE(ct.addNode(active));

    NodeEntry backup = {};
    std::strncpy(backup.id, CORE_B, UUID_LEN - 1);
    backup.role = NODE_ROLE_CORE;
    backup.status = NODE_STATUS_ONLINE;
    CHECK_TRUE(ct.addNode(backup));

    CHECK_TRUE(should_promote_backup_on_core_will(ct, CORE_B, CORE_A));
    CHECK_FALSE(should_promote_backup_on_core_will(ct, CORE_B, CORE_B));

    end_test("TC-13: should_promote_backup_on_core_will — active에만 반응");
}

// ── TC-14: should_promote_backup_on_core_will — active 미확정 시 보수적 허용 ─

static void tc_should_promote_backup_without_active_snapshot() {
    begin_test("TC-14: should_promote_backup_on_core_will — active 미확정 시 보수적 허용");

    ConnectionTableManager ct;
    ct.init("", CORE_B);

    CHECK_TRUE(should_promote_backup_on_core_will(ct, CORE_B, CORE_A));
    CHECK_FALSE(should_promote_backup_on_core_will(ct, CORE_B, CORE_B));

    end_test("TC-14: should_promote_backup_on_core_will — active 미확정 시 보수적 허용");
}

// ── TC-15: duplicate endpoint registration — 이전 online row를 offline 처리 ─

static void tc_duplicate_endpoint_registration_marks_old_row_offline() {
    begin_test("TC-15: duplicate endpoint registration — 이전 online row를 offline 처리");

    ConnectionTableManager ct;
    ct.init(CORE_A, "");

    NodeEntry old_edge = {};
    std::strncpy(old_edge.id, NODE_1, UUID_LEN - 1);
    old_edge.role = NODE_ROLE_NODE;
    std::strncpy(old_edge.ip, "192.168.0.9", IP_LEN - 1);
    old_edge.port = 1883;
    old_edge.status = NODE_STATUS_ONLINE;
    old_edge.hop_to_core = 1;
    CHECK_TRUE(ct.addNode(old_edge));

    CHECK_TRUE(mark_duplicate_endpoint_nodes_offline(
        ct,
        "ffffffff-0000-0000-0000-000000000010",
        "192.168.0.9",
        1883));

    auto old_edge_after = ct.findNode(NODE_1);
    CHECK_TRUE(old_edge_after.has_value());
    CHECK_EQ(old_edge_after->status, NODE_STATUS_OFFLINE);

    CHECK_FALSE(mark_duplicate_endpoint_nodes_offline(
        ct,
        "ffffffff-0000-0000-0000-000000000010",
        "192.168.0.9",
        1883));

    end_test("TC-15: duplicate endpoint registration — 이전 online row를 offline 처리");
}

// ── TC-16: setNodeStatus — 동일 상태 재적용은 no-op ────────────────────────

static void tc_set_node_status_is_idempotent() {
    begin_test("TC-16: setNodeStatus — 동일 상태 재적용은 no-op");

    ConnectionTableManager ct;
    ct.init("", "");

    NodeEntry edge = {};
    std::strncpy(edge.id, NODE_1, UUID_LEN - 1);
    edge.role = NODE_ROLE_NODE;
    edge.status = NODE_STATUS_OFFLINE;
    CHECK_TRUE(ct.addNode(edge));

    int version_before = ct.snapshot().version;
    CHECK_FALSE(ct.setNodeStatus(NODE_1, NODE_STATUS_OFFLINE));
    CHECK_EQ(ct.snapshot().version, version_before);

    CHECK_TRUE(ct.setNodeStatus(NODE_1, NODE_STATUS_ONLINE));
    CHECK_EQ(ct.findNode(NODE_1)->status, NODE_STATUS_ONLINE);

    end_test("TC-16: setNodeStatus — 동일 상태 재적용은 no-op");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    tc_parse_ip_port_normal();
    tc_parse_ip_port_invalid();
    tc_make_alert_topic();
    tc_dedup_basic();
    tc_dedup_cap();
    tc_edge_registration_roundtrip();
    tc_merge_disjoint();
    tc_merge_latest_snapshot_wins();
    tc_add_link_idempotent();
    tc_merge_backup_registration_equal_version();
    tc_merge_backup_registration_preserves_active_online_edges();
    tc_promote_core_after_failover_marks_failed_active_offline();
    tc_should_promote_backup_on_active_only();
    tc_should_promote_backup_without_active_snapshot();
    tc_duplicate_endpoint_registration_marks_old_row_offline();
    tc_set_node_status_is_idempotent();

    printf("══════════════════════════════════════\n");
    printf("  결과: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════\n");
    return g_fail == 0 ? 0 : 1;
}
