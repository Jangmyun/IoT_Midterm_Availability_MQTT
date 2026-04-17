#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "core-edge-bootstrap" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"

start_subscriber "$sub_log" "campus/monitor/topology" "campus/monitor/status/#" >/dev/null
sleep 1

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

start_edge "$edge_log" >/dev/null
if ! wait_for_pattern "$edge_log" '\[edge\] connected to core' 10; then
  show_file_tail "$edge_log"
  show_file_tail "$core_log"
  die "edge did not connect to core"
fi

if ! wait_for_pattern "$edge_log" '\[edge\] registered to core:' 10; then
  show_file_tail "$edge_log"
  die "edge did not publish registration status"
fi

edge_id="$(extract_edge_id "$edge_log")"
core_id="$(extract_core_id "$core_log")"

if ! wait_for_pattern "$sub_log" 'campus/monitor/topology' 10; then
  show_file_tail "$sub_log"
  die "topology topic was not observed"
fi

if ! wait_for_pattern "$sub_log" "campus/monitor/status/$edge_id" 10; then
  show_file_tail "$sub_log"
  die "edge status topic was not observed"
fi

log "core bootstrap ok: core_id=$core_id"
log "edge bootstrap ok: edge_id=$edge_id"
log "logs kept in $TEST_RUN_DIR until script exit"
