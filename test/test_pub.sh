#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
  cat <<'EOF'
Usage: ./test/test_pub.sh <case>

[client/smoke] 웹 클라이언트 렌더링 검증 (mosquitto_pub만 필요):
  topology
  event_intrusion
  event_motion
  event_door_forced
  node_down
  node_up
  core_switch
  lwt
  clear_topology

[client/edge-cases] 웹 클라이언트 파싱·방어 로직 검증:
  duplicate_event
  stale_topology
  invalid_uuid
  missing_priority

[core] core_broker 동작 검증 (BUILD_DIR 또는 CORE_BINARY 필요):
  core_bootstrap
  core_lwt

[edge] edge_broker 동작 검증 (BUILD_DIR 또는 EDGE_BINARY 필요):
  edge_ping_pong
  edge_lwt

[전체 실행]:
  all_client      client/smoke + client/edge-cases 전체
  all_core        core/ 전체 (바이너리 없으면 SKIP)
  all_edge        edge/ 전체 (바이너리 없으면 SKIP)
  all             전체 (all_client + all_core + all_edge)

[기타]:
  list
  help

공통 환경 변수:
  MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD
  CORE_A_IP, CORE_B_IP, NODE_1_IP, NODE_2_IP
  BUILD_DIR, CORE_BINARY, EDGE_BINARY, EDGE_NODE_PORT
EOF
}

list_cases() {
  cat <<'EOF'
client/smoke/topology
client/smoke/event_intrusion
client/smoke/event_motion
client/smoke/event_door_forced
client/smoke/node_down
client/smoke/node_up
client/smoke/core_switch
client/smoke/lwt
client/smoke/clear_topology
client/edge-cases/duplicate_event
client/edge-cases/stale_topology
client/edge-cases/invalid_uuid
client/edge-cases/missing_priority
core/bootstrap
core/lwt
edge/ping_pong
edge/lwt
EOF
}

run_all_client() {
  "$SCRIPT_DIR/client/smoke/90_all_smoke.sh"
  sleep 1
  "$SCRIPT_DIR/client/edge-cases/01_duplicate_event.sh"
  sleep 1
  "$SCRIPT_DIR/client/edge-cases/02_stale_topology.sh"
  sleep 1
  "$SCRIPT_DIR/client/edge-cases/03_invalid_event_uuid.sh"
  sleep 1
  "$SCRIPT_DIR/client/edge-cases/04_missing_priority.sh"
  printf '[test] all_client completed\n'
}

# 바이너리 존재 여부 확인 후 실행, 없으면 SKIP
try_run_binary_test() {
  local script="$1"
  local label="$2"

  # BUILD_DIR 또는 CORE_BINARY/EDGE_BINARY 중 하나라도 있으면 시도
  if [[ -z "${CORE_BINARY:-}" && -z "${EDGE_BINARY:-}" ]]; then
    local build_dir="${BUILD_DIR:-$(cd "$SCRIPT_DIR/.." && pwd)/build}"
    if [[ ! -x "$build_dir/core_broker" && ! -x "$(cd "$SCRIPT_DIR/.." && pwd)/broker/build/core_broker" ]]; then
      printf '[test][skip] %s — core_broker not found (set BUILD_DIR or CORE_BINARY)\n' "$label"
      return 0
    fi
  fi

  "$script"
}

run_all_core() {
  try_run_binary_test "$SCRIPT_DIR/core/01_bootstrap.sh" "core/01_bootstrap"
  try_run_binary_test "$SCRIPT_DIR/core/02_lwt.sh"       "core/02_lwt"
  printf '[test] all_core completed\n'
}

run_all_edge() {
  try_run_binary_test "$SCRIPT_DIR/edge/01_ping_pong.sh" "edge/01_ping_pong"
  try_run_binary_test "$SCRIPT_DIR/edge/02_lwt.sh"       "edge/02_lwt"
  printf '[test] all_edge completed\n'
}

case "${1:-help}" in
  # client/smoke
  topology)           exec "$SCRIPT_DIR/client/smoke/01_topology.sh" ;;
  event_intrusion)    exec "$SCRIPT_DIR/client/smoke/02_event_intrusion.sh" ;;
  event_motion)       exec "$SCRIPT_DIR/client/smoke/03_event_motion.sh" ;;
  event_door_forced)  exec "$SCRIPT_DIR/client/smoke/04_event_door_forced.sh" ;;
  node_down)          exec "$SCRIPT_DIR/client/smoke/05_node_down.sh" ;;
  node_up)            exec "$SCRIPT_DIR/client/smoke/06_node_up.sh" ;;
  core_switch)        exec "$SCRIPT_DIR/client/smoke/07_core_switch.sh" ;;
  lwt)                exec "$SCRIPT_DIR/client/smoke/08_core_lwt.sh" ;;
  clear_topology)     exec "$SCRIPT_DIR/client/smoke/99_clear_topology.sh" ;;
  # client/edge-cases
  duplicate_event)    exec "$SCRIPT_DIR/client/edge-cases/01_duplicate_event.sh" ;;
  stale_topology)     exec "$SCRIPT_DIR/client/edge-cases/02_stale_topology.sh" ;;
  invalid_uuid)       exec "$SCRIPT_DIR/client/edge-cases/03_invalid_event_uuid.sh" ;;
  missing_priority)   exec "$SCRIPT_DIR/client/edge-cases/04_missing_priority.sh" ;;
  # core
  core_bootstrap)     exec "$SCRIPT_DIR/core/01_bootstrap.sh" ;;
  core_lwt)           exec "$SCRIPT_DIR/core/02_lwt.sh" ;;
  # edge
  edge_ping_pong)     exec "$SCRIPT_DIR/edge/01_ping_pong.sh" ;;
  edge_lwt)           exec "$SCRIPT_DIR/edge/02_lwt.sh" ;;
  # 전체 실행
  all_client)         run_all_client ;;
  all_core)           run_all_core ;;
  all_edge)           run_all_edge ;;
  all)
    run_all_client
    run_all_core
    run_all_edge
    printf '[test] all tests completed\n'
    ;;
  list)               list_cases ;;
  help|-h|--help)     usage ;;
  *)
    usage >&2
    exit 1
    ;;
esac
