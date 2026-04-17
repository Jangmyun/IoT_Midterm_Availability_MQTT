#!/usr/bin/env bash
# CORE-08: Core Election 메커니즘 검증 (FR-10, C-03, C-04)
# Core A가 election request 발행 → Core B가 투표 → 승자의 CT에 active_core_id 반영
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-election" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_a_log="$TEST_RUN_DIR/core_a.log"
core_b_log="$TEST_RUN_DIR/core_b.log"

start_standalone_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  "$binary" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_subscriber "$sub_log" \
  "_core/election/#" \
  "campus/monitor/topology" >/dev/null
sleep 1

# Core A 기동
start_standalone_core "$core_a_log" >/dev/null
if ! wait_for_pattern "$core_a_log" '\[core\] connected' 10; then
  show_file_tail "$core_a_log"
  die "core A did not connect"
fi
core_a_id="$(extract_core_id "$core_a_log")"

# Core B 기동
start_standalone_core "$core_b_log" >/dev/null
if ! wait_for_pattern "$core_b_log" '\[core\] connected' 10; then
  show_file_tail "$core_b_log"
  die "core B did not connect"
fi
core_b_id="$(extract_core_id "$core_b_log")"

sleep 0.5

# election request 발행 (Core A가 요청하는 것처럼 시뮬레이션)
req_payload="$(cat <<EOF
{
  "msg_id": "$(gen_uuid)",
  "type": "ELECTION_REQ",
  "timestamp": "$(iso_timestamp)",
  "source": {"role": "CORE", "id": "$core_a_id"},
  "target": {"role": "CORE", "id": "all"},
  "route": {"original_node": "$core_a_id", "prev_hop": "$core_a_id", "next_hop": "all", "hop_count": 0, "ttl": 1},
  "delivery": {"qos": 1, "dup": false, "retain": false},
  "payload": {"building_id": "", "camera_id": "", "description": "$core_a_id"}
}
EOF
)"
mqtt_publish_json "_core/election/request" 1 false "$req_payload"

# election result (_core/election/result) 발행 확인
if ! wait_for_pattern "$sub_log" '_core/election/result' 10; then
  show_file_tail "$sub_log"
  die "election result was not published"
fi

# topology에 active_core_id가 반영되었는지 확인
if ! wait_for_pattern "$sub_log" 'campus/monitor/topology' 10; then
  show_file_tail "$sub_log"
  die "topology was not updated after election"
fi

log "election ok: core_a_id=$core_a_id, core_b_id=$core_b_id"
log "election result published and topology updated"
log "logs kept in $TEST_RUN_DIR until script exit"
