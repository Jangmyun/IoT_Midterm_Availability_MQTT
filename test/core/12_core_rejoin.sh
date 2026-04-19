#!/usr/bin/env bash
# TC-12 CORE-REJOIN: Core 재시작 후 동일 UUID 사용 및 Backup 재진입 검증
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "core-rejoin" >/dev/null
setup_cleanup_trap

CORE_ID_FILE="/tmp/core_id_${MQTT_PORT}.txt"
rm -f "$CORE_ID_FILE"

core_log1="$TEST_RUN_DIR/core1.log"
core_log2="$TEST_RUN_DIR/core2.log"
core_log3="$TEST_RUN_DIR/core3.log"

binary="$(core_binary)"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 1: Active Core 최초 기동 → core_id 파일 생성 확인
# ─────────────────────────────────────────────────────────────────────────────
log "STEP 1: Starting Active Core (first boot)"
"$binary" "$MQTT_HOST" "$MQTT_PORT" >"$core_log1" 2>&1 &
CORE1_PID=$!
register_pid "$CORE1_PID"

if ! wait_for_pattern "$core_log1" '\[core\] connected' 10; then
  show_file_tail "$core_log1"
  die "core1 did not connect"
fi

if [[ ! -f "$CORE_ID_FILE" ]]; then
  show_file_tail "$core_log1"
  die "core_id file was not created: $CORE_ID_FILE"
fi

UUID1="$(cat "$CORE_ID_FILE")"
log "core_id created: $UUID1"

if ! wait_for_pattern "$core_log1" '\[core\] generated new core_id' 5; then
  show_file_tail "$core_log1"
  die "generated new core_id log not found"
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 2: Core SIGKILL → 재시작 후 동일 UUID 사용 확인
# ─────────────────────────────────────────────────────────────────────────────
log "STEP 2: SIGKILL core, then restart"
kill_forcefully "$CORE1_PID"
sleep 0.5

"$binary" "$MQTT_HOST" "$MQTT_PORT" >"$core_log2" 2>&1 &
CORE2_PID=$!
register_pid "$CORE2_PID"

if ! wait_for_pattern "$core_log2" '\[core\] connected' 10; then
  show_file_tail "$core_log2"
  die "core2 did not connect"
fi

if ! wait_for_pattern "$core_log2" '\[core\] restored core_id from file' 5; then
  show_file_tail "$core_log2"
  die "core2 did not restore core_id from file"
fi

UUID2="$(cat "$CORE_ID_FILE")"
if [[ "$UUID1" != "$UUID2" ]]; then
  die "core_id changed after restart: $UUID1 → $UUID2"
fi
log "UUID preserved after restart: $UUID1 ✓"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 3: 기존 Active를 SIGKILL 후 Backup 모드로 재진입 — UUID 동일 확인
# ─────────────────────────────────────────────────────────────────────────────
log "STEP 3: Restart as Backup mode — UUID must be preserved"
kill_forcefully "$CORE2_PID"
sleep 0.5

# Backup 모드: argc=5 (active_core_ip active_core_port)
"$binary" "$MQTT_HOST" "$MQTT_PORT" "$MQTT_HOST" "$MQTT_PORT" \
  >"$core_log3" 2>&1 &
CORE3_PID=$!
register_pid "$CORE3_PID"

if ! wait_for_pattern "$core_log3" '\[core\] connected' 10; then
  show_file_tail "$core_log3"
  die "core3 (backup mode) did not connect"
fi

if ! wait_for_pattern "$core_log3" '\[core\] restored core_id from file' 5; then
  show_file_tail "$core_log3"
  die "core3 (backup mode) did not restore core_id from file"
fi

UUID3="$(cat "$CORE_ID_FILE")"
if [[ "$UUID1" != "$UUID3" ]]; then
  die "core_id changed in backup mode: $UUID1 → $UUID3"
fi
log "UUID preserved in Backup mode: $UUID1 ✓"

log "TC-12 PASSED"
