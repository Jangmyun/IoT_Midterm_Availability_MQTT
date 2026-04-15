#!/usr/bin/env bash
# test_pub.sh — mosquitto_pub으로 client UI 수동 테스트
# 사용법: ./test_pub.sh [명령]
#
#   ./test_pub.sh topology          # Connection Table 전송 (retained)
#   ./test_pub.sh event_intrusion   # HIGH 우선순위 침입 이벤트
#   ./test_pub.sh event_motion      # MOTION 이벤트
#   ./test_pub.sh node_down         # node-1 OFFLINE 알림
#   ./test_pub.sh node_up           # node-1 복구 알림
#   ./test_pub.sh core_switch       # Active Core 교체 알림 (재연결 배너 뜨는지 확인)
#   ./test_pub.sh lwt               # Core LWT 수신 (재연결 배너 뜨는지 확인)
#   ./test_pub.sh all               # topology 후 이벤트 3개 순서대로 전송

PUB="mosquitto_pub -h localhost -p 1883"

# ── ID 상수 ────────────────────────────────────────────────────
CORE_A="aaaaaaaa-0000-0000-0000-000000000001"
CORE_B="bbbbbbbb-0000-0000-0000-000000000002"
NODE_1="cccccccc-0000-0000-0000-000000000003"
NODE_2="dddddddd-0000-0000-0000-000000000004"

# ── M-04: Connection Table ──────────────────────────────────────
send_topology() {
  $PUB -t "campus/monitor/topology" -r -q 1 -m '{
    "version": 1,
    "last_update": "2026-04-13T12:00:00",
    "active_core_id": "'"$CORE_A"'",
    "backup_core_id": "'"$CORE_B"'",
    "node_count": 4,
    "nodes": [
      {"id":"'"$CORE_A"'","role":"CORE","ip":"127.0.0.1","port":1883,"status":"ONLINE","hop_to_core":0},
      {"id":"'"$CORE_B"'","role":"CORE","ip":"127.0.0.2","port":1883,"status":"ONLINE","hop_to_core":1},
      {"id":"'"$NODE_1"'","role":"NODE","ip":"10.0.0.3","port":1883,"status":"ONLINE","hop_to_core":2},
      {"id":"'"$NODE_2"'","role":"NODE","ip":"10.0.0.4","port":1883,"status":"ONLINE","hop_to_core":2}
    ],
    "link_count": 3,
    "links": [
      {"from_id":"'"$CORE_A"'","to_id":"'"$CORE_B"'","rtt_ms":1.2},
      {"from_id":"'"$CORE_A"'","to_id":"'"$NODE_1"'","rtt_ms":4.7},
      {"from_id":"'"$CORE_A"'","to_id":"'"$NODE_2"'","rtt_ms":8.1}
    ]
  }'
  echo "[OK] topology 전송 (retained)"
}

# ── D-01: INTRUSION 이벤트 (HIGH) ──────────────────────────────
send_event_intrusion() {
  $PUB -t "campus/data/INTRUSION" -q 1 -m '{
    "msg_id": "evt-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "INTRUSION",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "priority": "HIGH",
    "source": {"role":"NODE","id":"'"$NODE_1"'"},
    "target": {"role":"CORE","id":"'"$CORE_A"'"},
    "route": {"original_node":"'"$NODE_1"'","prev_hop":"'"$NODE_1"'","next_hop":"'"$CORE_A"'","hop_count":1,"ttl":5},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"bldg-a","camera_id":"cam-01","description":"침입 감지 — 정문 CCTV"}
  }'
  echo "[OK] INTRUSION (HIGH) 이벤트 전송"
}

# ── D-02: MOTION 이벤트 (건물별 세분화) ───────────────────────
send_event_motion() {
  $PUB -t "campus/data/MOTION/bldg-b" -q 1 -m '{
    "msg_id": "evt-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "MOTION",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "source": {"role":"NODE","id":"'"$NODE_2"'"},
    "target": {"role":"CORE","id":"'"$CORE_A"'"},
    "route": {"original_node":"'"$NODE_2"'","prev_hop":"'"$NODE_2"'","next_hop":"'"$CORE_A"'","hop_count":1,"ttl":5},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"bldg-b","camera_id":"cam-05","description":"복도 움직임 감지"}
  }'
  echo "[OK] MOTION 이벤트 전송 (bldg-b)"
}

# ── A-01: Node OFFLINE 알림 ─────────────────────────────────────
send_node_down() {
  $PUB -t "campus/alert/node_down/$NODE_1" -q 1 -m '{
    "msg_id": "alrt-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "STATUS",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "source": {"role":"CORE","id":"'"$CORE_A"'"},
    "target": {"role":"NODE","id":"'"$NODE_1"'"},
    "route": {"original_node":"'"$CORE_A"'","prev_hop":"'"$CORE_A"'","next_hop":"'"$NODE_1"'","hop_count":0,"ttl":1},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"bldg-a","camera_id":"","description":"Node OFFLINE"}
  }'
  echo "[OK] node_down 알림 전송 (bldg-a)"
}

# ── A-02: Node 복구 알림 ────────────────────────────────────────
send_node_up() {
  $PUB -t "campus/alert/node_up/$NODE_1" -q 1 -m '{
    "msg_id": "alrt-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "STATUS",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "source": {"role":"CORE","id":"'"$CORE_A"'"},
    "target": {"role":"NODE","id":"'"$NODE_1"'"},
    "route": {"original_node":"'"$CORE_A"'","prev_hop":"'"$CORE_A"'","next_hop":"'"$NODE_1"'","hop_count":0,"ttl":1},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"bldg-a","camera_id":"","description":"Node 복구 완료"}
  }'
  echo "[OK] node_up 알림 전송 (bldg-a)"
}

# ── A-03: Active Core 교체 알림 (재연결 배너 테스트) ─────────────
send_core_switch() {
  $PUB -t "campus/alert/core_switch" -q 1 -m '{
    "msg_id": "sw-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "STATUS",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "source": {"role":"CORE","id":"'"$CORE_B"'"},
    "target": {"role":"NODE","id":"all"},
    "route": {"original_node":"'"$CORE_B"'","prev_hop":"'"$CORE_B"'","next_hop":"all","hop_count":0,"ttl":1},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"","camera_id":"","description":"Active Core 교체: CORE_B가 새 Active Core"}
  }'
  echo "[OK] core_switch 알림 전송 → 재연결 배너 확인"
}

# ── W-01: Core LWT (재연결 배너 테스트) ────────────────────────
send_lwt() {
  $PUB -t "campus/will/core/$CORE_A" -q 1 -m '{
    "msg_id": "lwt-'"$(date +%s%N | md5 | head -c 8)"'",
    "type": "LWT_CORE",
    "timestamp": "'"$(date -u +%Y-%m-%dT%H:%M:%S)"'",
    "source": {"role":"CORE","id":"'"$CORE_A"'"},
    "target": {"role":"NODE","id":"all"},
    "route": {"original_node":"'"$CORE_A"'","prev_hop":"'"$CORE_A"'","next_hop":"all","hop_count":0,"ttl":1},
    "delivery": {"qos":1,"dup":false,"retain":false},
    "payload": {"building_id":"","camera_id":"","description":"Core A 비정상 종료"}
  }'
  echo "[OK] LWT_CORE 전송 → 재연결 배너 확인"
}

# ── 전체 시나리오 ────────────────────────────────────────────────
send_all() {
  send_topology
  sleep 1
  send_event_intrusion
  sleep 0.5
  send_event_motion
  sleep 0.5
  send_node_down
  sleep 1
  send_node_up
  echo "[OK] 전체 시나리오 완료"
}

# ── 명령 분기 ────────────────────────────────────────────────────
case "${1}" in
  topology)          send_topology ;;
  event_intrusion)   send_event_intrusion ;;
  event_motion)      send_event_motion ;;
  node_down)         send_node_down ;;
  node_up)           send_node_up ;;
  core_switch)       send_core_switch ;;
  lwt)               send_lwt ;;
  all)               send_all ;;
  *)
    echo "사용법: $0 {topology|event_intrusion|event_motion|node_down|node_up|core_switch|lwt|all}"
    exit 1
    ;;
esac
