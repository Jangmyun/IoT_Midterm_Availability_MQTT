#!/usr/bin/env bash
# EDGE-03: campus/alert/core_switch 수신 → Edge 새 Active Core 재연결 검증 (FR-05, A-03)
# core_switch payload(ip:port) 파싱 후 mosq_core 재연결 시도 로그 확인
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto mosquitto_pub mosquitto_sub
make_run_dir "edge-core-switch" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"
replacement_broker_log="$TEST_RUN_DIR/replacement-broker.log"

# 현재 연결 중인 Core와 다른 주소
NEW_CORE_IP="$MQTT_HOST"
NEW_CORE_PORT="${EDGE_CORE_SWITCH_NEW_PORT:-$((20000 + (${RANDOM:-42} % 10000)))}"

start_replacement_broker() {
  mosquitto -p "$NEW_CORE_PORT" -v >"$replacement_broker_log" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

start_edge "$edge_log" >/dev/null
if ! wait_for_pattern "$edge_log" '\[edge\] registered to' 10; then
  show_file_tail "$edge_log"
  die "edge did not finish registration"
fi

start_replacement_broker >/dev/null
sleep 1

core_connect_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"

# core_switch 발행: description = "NEW_IP:NEW_PORT" 형식의 STATUS 메시지
sw_payload="$(emit_status_json "CORE" "$(gen_uuid)" "NODE" "all" \
  "${NEW_CORE_IP}:${NEW_CORE_PORT}")"
mqtt_publish_json "campus/alert/core_switch" 1 false "$sw_payload"

# Edge가 새 Core 주소로 재연결 시도하는지 확인
if ! wait_for_pattern "$edge_log" \
  "core_switch: reconnecting to ${NEW_CORE_IP}:${NEW_CORE_PORT}" 10; then
  show_file_tail "$edge_log"
  die "edge did not attempt reconnect after core_switch"
fi

# 새 Core로 실제 재연결 확인
deadline=$((SECONDS + 10))
while (( SECONDS < deadline )); do
  current_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"
  if (( current_count > core_connect_count )); then
    break
  fi
  sleep 1
done

current_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"
if (( current_count <= core_connect_count )); then
  show_file_tail "$edge_log"
  show_file_tail "$replacement_broker_log"
  die "edge did not reconnect to replacement core after core_switch"
fi

log "core_switch ok: edge reconnecting to ${NEW_CORE_IP}:${NEW_CORE_PORT}"
log "logs kept in $TEST_RUN_DIR until script exit"
