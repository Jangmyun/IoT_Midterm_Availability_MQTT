#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "core-lwt" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"

start_subscriber "$sub_log" "campus/will/core/#" >/dev/null
sleep 1

core_pid="$(start_core "$core_log")"
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

core_id="$(extract_core_id "$core_log")"
kill_forcefully "$core_pid"

if ! wait_for_pattern "$sub_log" "campus/will/core/$core_id" 10; then
  show_file_tail "$sub_log"
  die "core LWT topic was not observed"
fi

if ! wait_for_pattern "$sub_log" "$BACKUP_CORE_IP:$BACKUP_CORE_PORT" 10; then
  show_file_tail "$sub_log"
  die "backup core endpoint not found in LWT payload"
fi

log "core LWT ok: core_id=$core_id backup=$BACKUP_CORE_IP:$BACKUP_CORE_PORT"
log "logs kept in $TEST_RUN_DIR until script exit"
