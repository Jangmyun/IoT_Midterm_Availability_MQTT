#!/usr/bin/env bash
# CORE-06: Active Core SIGKILL → LWT 발행 → Backup Core가 core_switch 알림 발행 검증
#          (FR-04, FR-05, FR-14, A-03, 시나리오 5.4)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "core-switch" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
active_log="$TEST_RUN_DIR/active.log"
backup_log="$TEST_RUN_DIR/backup.log"

# Active Core: argc=3 (is_backup=false)
start_active_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  "$binary" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

# Backup Core: argc=5 (is_backup=true), peer → Active's broker (same mosquitto)
start_backup_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  "$binary" "$MQTT_HOST" "$MQTT_PORT" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_subscriber "$sub_log" \
  "campus/will/core/#" \
  "campus/alert/core_switch" >/dev/null
sleep 1

active_pid="$(start_active_core "$active_log")"
if ! wait_for_pattern "$active_log" '\[core\] connected (ACTIVE)' 10; then
  show_file_tail "$active_log"
  die "active core did not connect"
fi

active_core_id="$(extract_core_id "$active_log")"

start_backup_core "$backup_log" >/dev/null
if ! wait_for_pattern "$backup_log" '\[core\] connected (BACKUP)' 10; then
  show_file_tail "$backup_log"
  die "backup core did not connect"
fi

if ! wait_for_pattern "$backup_log" '\[core/backup\] connected to active broker' 10; then
  show_file_tail "$backup_log"
  die "backup core did not connect to active broker via peer"
fi

# Active Core 강제 종료 → LWT 발행
kill_forcefully "$active_pid"

if ! wait_for_pattern "$sub_log" "campus/will/core/$active_core_id" 15; then
  show_file_tail "$sub_log"
  die "active core LWT was not published"
fi

# Backup이 LWT 수신 후 campus/alert/core_switch 발행 확인
if ! wait_for_pattern "$sub_log" 'campus/alert/core_switch' 15; then
  show_file_tail "$sub_log"
  die "campus/alert/core_switch was not published by backup core"
fi

# core_switch payload에 Backup의 IP:Port 포함 확인
if ! wait_for_pattern "$sub_log" "$MQTT_HOST" 5; then
  show_file_tail "$sub_log"
  die "core_switch payload does not contain backup core IP"
fi

log "core switch ok: active_core_id=$active_core_id"
log "backup core published campus/alert/core_switch with its own endpoint"
log "logs kept in $TEST_RUN_DIR until script exit"
