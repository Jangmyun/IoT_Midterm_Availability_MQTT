#!/usr/bin/env bash
# CORE-03: Node LWT → OFFLINE 마킹 → node_down 알림 발행 검증 (FR-06, A-01, 시나리오 5.5)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-node-offline" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"

node_id="$NODE_1_ID"

start_subscriber "$sub_log" \
  "campus/monitor/topology" \
  "campus/alert/node_down/#" >/dev/null
sleep 1

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

# M-03: 노드 등록 (description = "ip:port" 형식)
reg_payload="$(emit_status_json "NODE" "$node_id" "CORE" "" "127.0.0.1:2883" "" "" "$(gen_uuid)")"
mqtt_publish_json "campus/monitor/status/$node_id" 1 false "$reg_payload"

if ! wait_for_pattern "$core_log" "edge registered: $node_id" 10; then
  show_file_tail "$core_log"
  die "core did not register node $node_id"
fi

# 등록 후 topology broadcast 에 node가 포함되어야 함
if ! wait_for_pattern "$sub_log" 'campus/monitor/topology' 10; then
  show_file_tail "$sub_log"
  die "topology was not broadcast after node registration"
fi

# W-02: 노드 LWT 시뮬레이션 (강제 종료 흉내)
lwt_payload="$(emit_status_json "NODE" "$node_id" "CORE" "" "" "" "" "$(gen_uuid)")"
mqtt_publish_json "campus/will/node/$node_id" 1 false "$lwt_payload"

if ! wait_for_pattern "$sub_log" "campus/alert/node_down/$node_id" 10; then
  show_file_tail "$sub_log"
  die "node_down alert was not published for $node_id"
fi

if ! wait_for_pattern "$sub_log" 'OFFLINE' 10; then
  show_file_tail "$sub_log"
  die "node_down alert payload does not contain OFFLINE status"
fi

log "node offline ok: node_id=$node_id"
log "logs kept in $TEST_RUN_DIR until script exit"
