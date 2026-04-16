#!/usr/bin/env bash
# CORE-04: 동일 msg_id 이벤트 중복 발행 → Core가 한 번만 재발행하는지 검증 (FR-02, 시나리오 5.1)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-event-dedup" >/dev/null
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

# 동일 msg_id 로 이벤트 두 번 발행
fixed_msg_id="$(gen_uuid)"
payload="$(emit_event_json "MOTION" "LOW" "$NODE_1_ID" "" "bldg-a" "cam-01" "test dedup" "$fixed_msg_id")"

mqtt_publish_json "campus/data/MOTION" 1 false "$payload"
sleep 1
mqtt_publish_json "campus/data/MOTION" 1 false "$payload"

# Core가 첫 번째 이벤트를 재발행할 때까지 대기
if ! wait_for_pattern "$core_log" "event forwarded.*$fixed_msg_id" 10; then
  show_file_tail "$core_log"
  die "core did not forward the first event"
fi

# 충분히 기다린 후 재발행 횟수 확인 (1회여야 함)
sleep 2

count="$(grep -c "event forwarded.*$fixed_msg_id" "$core_log" || true)"
if [[ "$count" -ne 1 ]]; then
  show_file_tail "$core_log"
  die "core forwarded duplicate event: expected 1 forward, got $count"
fi

log "event dedup ok: msg_id=$fixed_msg_id forwarded exactly once"
log "logs kept in $TEST_RUN_DIR until script exit"
