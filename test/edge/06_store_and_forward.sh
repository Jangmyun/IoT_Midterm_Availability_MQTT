#!/usr/bin/env bash
# EDGE-06: Store-and-Forward 큐 저장 후 재연결 시 flush 검증 (FR-07)
# upstream broker 단절 상태에서 로컬 이벤트 수신 → 큐 적재 → broker 복구 후 재전송
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto mosquitto_pub mosquitto_sub
make_run_dir "edge-store-and-forward" >/dev/null
setup_cleanup_trap

binary="$(edge_binary)"
local_port="$MQTT_PORT"
upstream_port="${STORE_FORWARD_CORE_PORT:-$((20000 + (${RANDOM:-42} % 10000)))}"

upstream_broker_log="$TEST_RUN_DIR/upstream-broker.log"
upstream_sub_log="$TEST_RUN_DIR/upstream-subscriber.log"
core_log="$TEST_RUN_DIR/core.log"
edge_log="$TEST_RUN_DIR/edge.log"

start_upstream_broker() {
  local log_file="$1"
  mosquitto -p "$upstream_port" -v >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_upstream_subscriber() {
  local outfile="$1"
  mosquitto_sub -h "$MQTT_HOST" -p "$upstream_port" -v -t "campus/data/#" >"$outfile" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

upstream_broker_pid="$(start_upstream_broker "$upstream_broker_log")"
sleep 1
if ! kill -0 "$upstream_broker_pid" 2>/dev/null; then
  show_file_tail "$upstream_broker_log"
  die "upstream broker failed to start on port $upstream_port"
fi

orig_mqtt_port="$MQTT_PORT"
MQTT_PORT="$upstream_port"
EDGE_NODE_PORT="$local_port"
EDGE_CORE_PORT="$upstream_port"

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  show_file_tail "$upstream_broker_log"
  die "core did not connect to upstream broker"
fi

"$binary" "$MQTT_HOST" "$local_port" "$MQTT_HOST" "$upstream_port" >"$edge_log" 2>&1 &
register_pid "$!"

if ! wait_for_pattern "$edge_log" '\[edge\] registered to' 10; then
  show_file_tail "$edge_log"
  die "edge did not finish registration"
fi

kill_forcefully "$upstream_broker_pid"

if ! wait_for_pattern "$edge_log" 'disconnected from core' 20; then
  show_file_tail "$edge_log"
  die "edge did not detect upstream broker disconnect"
fi

msg_id="$(gen_uuid)"
local_payload='{"building_id":"bldg-a","camera_id":"cam-01","description":"store-and-forward event"}'
mosquitto_pub -h "$MQTT_HOST" -p "$local_port" -t "campus/data/INTRUSION" -m "$local_payload"

if ! wait_for_pattern "$edge_log" 'queued event for later delivery' 10; then
  show_file_tail "$edge_log"
  die "edge did not queue event while upstream was unavailable"
fi

core_connect_count="$(grep -c '\[core\] connected' "$core_log" || true)"
edge_connect_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"

upstream_broker_pid="$(start_upstream_broker "$upstream_broker_log")"
sleep 1
start_upstream_subscriber "$upstream_sub_log" >/dev/null
sleep 1

deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  current_count="$(grep -c '\[core\] connected' "$core_log" || true)"
  if (( current_count > core_connect_count )); then
    break
  fi
  sleep 1
done

current_count="$(grep -c '\[core\] connected' "$core_log" || true)"
if (( current_count <= core_connect_count )); then
  show_file_tail "$core_log"
  show_file_tail "$upstream_broker_log"
  die "core did not reconnect after upstream broker restart"
fi

deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  current_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"
  if (( current_count > edge_connect_count )); then
    break
  fi
  sleep 1
done

current_count="$(grep -c '\[edge\] connected to core' "$edge_log" || true)"
if (( current_count <= edge_connect_count )); then
  show_file_tail "$edge_log"
  die "edge did not reconnect to upstream broker"
fi

if ! wait_for_pattern "$edge_log" 'flushed one queued event' 15; then
  show_file_tail "$edge_log"
  die "edge did not flush queued event after reconnect"
fi

if ! wait_for_pattern "$upstream_sub_log" 'campus/data/INTRUSION' 15; then
  show_file_tail "$upstream_sub_log"
  die "upstream subscriber did not observe flushed event"
fi

log "store-and-forward ok: queued event flushed after reconnect"
log "logs kept in $TEST_RUN_DIR until script exit"
