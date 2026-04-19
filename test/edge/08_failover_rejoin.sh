#!/usr/bin/env bash
# EDGE-08: Active Core failover 후 edge 재기동 시 backup 경로 CT를 학습해
#          새 Active Core로 다시 붙고 이벤트 전달을 유지해야 한다.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_cmd mosquitto mosquitto_pub
make_run_dir "edge-failover-rejoin" >/dev/null
setup_cleanup_trap

core_bin="$(core_binary)"
edge_bin="$(edge_binary)"

active_broker_port="${EDGE_FAILOVER_ACTIVE_PORT:-$((21000 + (${RANDOM:-42} % 1000)))}"
backup_broker_port="${EDGE_FAILOVER_BACKUP_PORT:-$((active_broker_port + 1))}"
local_broker_port="${EDGE_FAILOVER_LOCAL_PORT:-$((active_broker_port + 2))}"
host="127.0.0.1"

active_broker_log="$TEST_RUN_DIR/active-broker.log"
backup_broker_log="$TEST_RUN_DIR/backup-broker.log"
local_broker_log="$TEST_RUN_DIR/local-broker.log"
active_core_log="$TEST_RUN_DIR/active-core.log"
backup_core_log="$TEST_RUN_DIR/backup-core.log"
edge_first_log="$TEST_RUN_DIR/edge-first.log"
edge_second_log="$TEST_RUN_DIR/edge-second.log"

start_broker() {
  local port="$1"
  local log_file="$2"
  mosquitto -p "$port" -v >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_active_core() {
  local log_file="$1"
  "$core_bin" "$host" "$active_broker_port" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_backup_core() {
  local log_file="$1"
  "$core_bin" "$host" "$backup_broker_port" "$host" "$active_broker_port" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_edge_instance() {
  local log_file="$1"
  "$edge_bin" "$host" "$local_broker_port" "$host" "$active_broker_port" "$host" "$backup_broker_port" \
    >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

publish_local_event() {
  local marker="$1"
  local payload
  payload="{\"building_id\":\"building-a\",\"camera_id\":\"cam-01\",\"description\":\"failover-rejoin-${marker}\"}"
  mosquitto_pub -h "$host" -p "$local_broker_port" \
    -t "campus/data/door/building-a/cam-01" \
    -m "$payload"
}

start_broker "$active_broker_port" "$active_broker_log" >/dev/null
start_broker "$backup_broker_port" "$backup_broker_log" >/dev/null
start_broker "$local_broker_port" "$local_broker_log" >/dev/null
sleep 1

active_core_pid="$(start_active_core "$active_core_log")"
if ! wait_for_pattern "$active_core_log" '\[core\] connected \(ACTIVE\)' 10; then
  show_file_tail "$active_core_log"
  die "active core did not connect"
fi

start_backup_core "$backup_core_log" >/dev/null
if ! wait_for_pattern "$backup_core_log" '\[core\] connected \(BACKUP\)' 10; then
  show_file_tail "$backup_core_log"
  die "backup core did not connect"
fi
if ! wait_for_pattern "$backup_core_log" '\[core/backup\] connected to active broker' 10; then
  show_file_tail "$backup_core_log"
  die "backup core did not connect to active broker"
fi

edge_pid="$(start_edge_instance "$edge_first_log")"
if ! wait_for_pattern "$edge_first_log" '\[edge\] registered to core:' 10; then
  show_file_tail "$edge_first_log"
  die "edge did not register to core on first boot"
fi
if ! wait_for_pattern "$edge_first_log" '\[edge\] registered to backup core:' 10; then
  show_file_tail "$edge_first_log"
  die "edge did not register to backup core on first boot"
fi

kill_forcefully "$active_core_pid"
if ! wait_for_pattern "$backup_core_log" '\[core/backup\] active core down: .+ promoting self' 15; then
  show_file_tail "$backup_core_log"
  die "backup core was not promoted after active core failure"
fi

publish_local_event "before-rejoin"
if ! wait_for_pattern "$backup_core_log" '\[core\] event forwarded: campus/data/door/building-a/cam-01' 15; then
  show_file_tail "$edge_first_log"
  show_file_tail "$backup_core_log"
  die "backup core did not receive event after failover"
fi

forwarded_before_rejoin="$(grep -c '\[core\] event forwarded: campus/data/door/building-a/cam-01' "$backup_core_log" || true)"

kill_forcefully "$edge_pid"
edge_pid="$(start_edge_instance "$edge_second_log")"

if ! wait_for_pattern "$edge_second_log" '\[edge\] connected to backup core' 10; then
  show_file_tail "$edge_second_log"
  die "restarted edge did not connect to backup broker"
fi

if ! wait_for_pattern "$edge_second_log" "active_core_id changed → reconnect to ${host}:${backup_broker_port}" 15; then
  show_file_tail "$edge_second_log"
  show_file_tail "$backup_core_log"
  die "restarted edge did not learn promoted active core from backup topology"
fi

publish_local_event "after-rejoin"

deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  current_count="$(grep -c '\[core\] event forwarded: campus/data/door/building-a/cam-01' "$backup_core_log" || true)"
  if (( current_count > forwarded_before_rejoin )); then
    break
  fi
  sleep 1
done

current_count="$(grep -c '\[core\] event forwarded: campus/data/door/building-a/cam-01' "$backup_core_log" || true)"
if (( current_count <= forwarded_before_rejoin )); then
  show_file_tail "$edge_second_log"
  show_file_tail "$backup_core_log"
  show_file_tail "$active_broker_log"
  die "restarted edge did not forward event to promoted active core"
fi

log "failover rejoin ok: restarted edge relearned active core via backup CT"
log "logs kept in $TEST_RUN_DIR until script exit"
