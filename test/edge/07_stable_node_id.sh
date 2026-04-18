#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "edge-stable-node-id" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
edge_log_a="$TEST_RUN_DIR/edge-a.log"
edge_log_b="$TEST_RUN_DIR/edge-b.log"

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

start_edge "$edge_log_a" >/dev/null
if ! wait_for_pattern "$edge_log_a" '\[edge\] registered to' 10; then
  show_file_tail "$edge_log_a"
  die "first edge start did not finish registration"
fi

edge_pid_a="${TEST_PIDS[$((${#TEST_PIDS[@]} - 1))]}"
edge_id_a="$(extract_edge_id "$edge_log_a")"
if [[ -z "$edge_id_a" ]]; then
  show_file_tail "$edge_log_a"
  die "failed to extract first edge id"
fi

kill_forcefully "$edge_pid_a"
sleep 1

start_edge "$edge_log_b" >/dev/null
if ! wait_for_pattern "$edge_log_b" '\[edge\] registered to' 10; then
  show_file_tail "$edge_log_b"
  die "second edge start did not finish registration"
fi

edge_id_b="$(extract_edge_id "$edge_log_b")"
if [[ -z "$edge_id_b" ]]; then
  show_file_tail "$edge_log_b"
  die "failed to extract second edge id"
fi

if [[ "$edge_id_a" != "$edge_id_b" ]]; then
  show_file_tail "$edge_log_a"
  show_file_tail "$edge_log_b"
  die "edge id changed across restart: $edge_id_a -> $edge_id_b"
fi

log "stable node id ok: edge_id=$edge_id_a"
log "logs kept in $TEST_RUN_DIR until script exit"
