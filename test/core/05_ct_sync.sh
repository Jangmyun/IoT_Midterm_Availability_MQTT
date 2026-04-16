#!/usr/bin/env bash
# CORE-05: Active Core가 CT_SYNC(retained) 발행 → Edge 등록 후 CT 갱신 재발행 검증 (FR-01, C-01, 시나리오 5.2)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-ct-sync" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"

# Active Core 시작 (argc=3 → is_backup=false)
start_active_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  "$binary" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

mqtt_clear_retained "_core/sync/connection_table" || true

start_subscriber "$sub_log" \
  "_core/sync/connection_table" \
  "campus/monitor/topology" >/dev/null
sleep 1

start_active_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected \(ACTIVE\)' 10; then
  show_file_tail "$core_log"
  die "active core did not connect"
fi

# Active Core 초기 CT_SYNC retained 발행 확인
if ! wait_for_pattern "$sub_log" '_core/sync/connection_table' 10; then
  show_file_tail "$sub_log"
  die "_core/sync/connection_table was not published"
fi

# M-03: edge 등록 → CT 갱신 후 CT_SYNC 재발행 확인
node_id="$NODE_1_ID"
reg_payload="$(emit_status_json "NODE" "$node_id" "CORE" "" "127.0.0.1:2883" "" "" "$(gen_uuid)")"
mqtt_publish_json "campus/monitor/status/$node_id" 1 false "$reg_payload"

if ! wait_for_pattern "$core_log" "edge registered: $node_id" 10; then
  show_file_tail "$core_log"
  die "active core did not register node $node_id"
fi

# 갱신된 CT_SYNC 재발행 확인 (version >= 2)
if ! wait_for_pattern "$sub_log" '"version": *[2-9]' 15; then
  show_file_tail "$sub_log"
  die "CT_SYNC was not re-published after node registration"
fi

log "ct_sync ok: Active Core publishes _core/sync/connection_table on change"
log "logs kept in $TEST_RUN_DIR until script exit"
