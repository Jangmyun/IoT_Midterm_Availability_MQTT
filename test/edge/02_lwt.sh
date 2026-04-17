#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "edge-lwt" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"

start_subscriber "$sub_log" "campus/will/node/#" >/dev/null
sleep 1

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

edge_pid="$(start_edge "$edge_log")"
if ! wait_for_pattern "$edge_log" '\[edge\] registered to' 10; then
  show_file_tail "$edge_log"
  die "edge did not finish registration"
fi

edge_id="$(extract_edge_id "$edge_log")"
kill_forcefully "$edge_pid"

if ! wait_for_pattern "$sub_log" "campus/will/node/$edge_id" 10; then
  show_file_tail "$sub_log"
  die "edge node LWT topic was not observed"
fi

log "edge LWT ok: edge_id=$edge_id"
log "note: core-side CT offline update still depends on unimplemented status ingestion"
log "logs kept in $TEST_RUN_DIR until script exit"
