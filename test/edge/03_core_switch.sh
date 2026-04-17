#!/usr/bin/env bash
# EDGE-03: campus/alert/core_switch 수신 → Edge 새 Active Core 재연결 검증 (FR-05, A-03)
# core_switch payload(ip:port) 파싱 후 mosq_core 재연결 시도 로그 확인
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "edge-core-switch" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"

# 현재 연결 중인 Core와 다른 주소 (실제로는 없는 포트 — 재연결 시도 로그만 확인)
NEW_CORE_IP="$MQTT_HOST"
NEW_CORE_PORT=19883

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

start_edge "$edge_log" >/dev/null
if ! wait_for_pattern "$edge_log" '\[edge\] registered:' 10; then
  show_file_tail "$edge_log"
  die "edge did not finish registration"
fi

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

# 기존 Core에서 disconnect 확인
if ! wait_for_pattern "$edge_log" 'disconnected from core' 10; then
  show_file_tail "$edge_log"
  die "edge did not disconnect from previous core"
fi

log "core_switch ok: edge reconnecting to ${NEW_CORE_IP}:${NEW_CORE_PORT}"
log "logs kept in $TEST_RUN_DIR until script exit"
