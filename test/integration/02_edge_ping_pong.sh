#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "edge-ping-pong" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"

start_subscriber "$sub_log" "campus/monitor/pong/#" >/dev/null
sleep 1

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

edge_id="$(extract_edge_id "$edge_log")"
payload="$(emit_ping_json "$PING_REQUESTER_ID" "$edge_id")"

mqtt_publish_json "campus/monitor/ping/$edge_id" 0 false "$payload"

if ! wait_for_pattern "$sub_log" "campus/monitor/pong/$PING_REQUESTER_ID" 10; then
  show_file_tail "$sub_log"
  show_file_tail "$edge_log"
  die "pong response was not observed"
fi

log "ping/pong ok: edge_id=$edge_id requester_id=$PING_REQUESTER_ID"
log "logs kept in $TEST_RUN_DIR until script exit"
