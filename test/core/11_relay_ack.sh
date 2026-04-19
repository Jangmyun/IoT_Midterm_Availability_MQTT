#!/usr/bin/env bash
# TC-11 RELAY-ACK: Core 가 이벤트 처리 후 campus/relay/ack/<msg_id> 를 발행하는지 검증
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-relay-ack" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
sub_log="$TEST_RUN_DIR/subscriber.log"

# ── 구독 시작 ────────────────────────────────────────────────────────────────
start_subscriber "$sub_log" "campus/relay/ack/#" "campus/data/#" >/dev/null
sleep 0.5

# ── Core 기동 ────────────────────────────────────────────────────────────────
start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

# ── 이벤트 발행 ──────────────────────────────────────────────────────────────
MSG_ID="$(gen_uuid)"
SRC_ID="$(gen_uuid)"
PAYLOAD="$(emit_event_json "INTRUSION" "HIGH" "$SRC_ID" "all" "bldg-a" "cam-01" "test event" "$MSG_ID")"

log "publishing INTRUSION event: msg_id=$MSG_ID"
mqtt_publish_json "campus/data/INTRUSION" 1 false "$PAYLOAD"

# ── relay/ack 수신 확인 ───────────────────────────────────────────────────────
ACK_PATTERN="campus/relay/ack/$MSG_ID"
log "waiting for relay/ack topic: $ACK_PATTERN"
if ! wait_for_pattern "$sub_log" "$ACK_PATTERN" 10; then
  show_file_tail "$sub_log" 20
  show_file_tail "$core_log" 20
  die "relay/ack was not observed for msg_id=$MSG_ID"
fi
log "relay/ack received: $ACK_PATTERN ✓"

# ── 중복 이벤트 → relay/ack 미발행 확인 ──────────────────────────────────────
log "publishing duplicate event (same msg_id): $MSG_ID"
# ack sub_log 의 해당 토픽 등장 횟수를 기록
COUNT_BEFORE=$(grep -c "$ACK_PATTERN" "$sub_log" || true)

mqtt_publish_json "campus/data/INTRUSION" 1 false "$PAYLOAD"
sleep 2

COUNT_AFTER=$(grep -c "$ACK_PATTERN" "$sub_log" || true)
if (( COUNT_AFTER > COUNT_BEFORE )); then
  show_file_tail "$sub_log" 20
  die "relay/ack was published for duplicate event (dedup failed)"
fi
log "duplicate event dedup ok: relay/ack count did not increase ✓"

log "TC-11 PASSED"
