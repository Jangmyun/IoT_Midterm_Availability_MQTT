#!/usr/bin/env bash
# CORE-10: CCTV 이벤트 종단간 전달 검증 (FR-03)
# publisher → campus/data/<event_type> → core 재발행 → subscriber 수신
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-event-e2e" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
core_log="$TEST_RUN_DIR/core.log"

start_subscriber "$sub_log" "campus/data/#" >/dev/null
sleep 1

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

msg_id="$(gen_uuid)"
payload="$(emit_event_json "INTRUSION" "HIGH" "$NODE_1_ID" "" "bldg-a" "cam-01" "end-to-end event" "$msg_id")"

mqtt_publish_json "campus/data/INTRUSION" 1 false "$payload"

if ! wait_for_pattern "$core_log" 'event forwarded: campus/data/INTRUSION' 10; then
  show_file_tail "$core_log"
  die "core did not forward intrusion event"
fi

if ! wait_for_pattern "$sub_log" "$msg_id" 10; then
  show_file_tail "$sub_log"
  die "subscriber did not receive published event"
fi

event_count="$(grep -c "$msg_id" "$sub_log" || true)"
if [[ "$event_count" -lt 2 ]]; then
  show_file_tail "$sub_log"
  die "expected original + forwarded event for msg_id=$msg_id, got $event_count deliveries"
fi

log "event e2e ok: msg_id=$msg_id received $event_count times"
log "logs kept in $TEST_RUN_DIR until script exit"
