#!/usr/bin/env bash
# EDGE-05: RTT 측정 및 Relay Node 선택 검증 (FR-08)
# 두 Edge가 Core에 등록 → CT 수신 → 상호 Ping/Pong → RTT 계산 → relay node 선택 확인
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_mqtt_tools
make_run_dir "edge-rtt-relay" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
edge1_log="$TEST_RUN_DIR/edge1.log"
edge2_log="$TEST_RUN_DIR/edge2.log"

binary="$(edge_binary)"

start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

# Edge1 기동
"$binary" "$MQTT_HOST" "$MQTT_PORT" "$MQTT_HOST" "$MQTT_PORT" \
  >"$edge1_log" 2>&1 &
register_pid "$!"

if ! wait_for_pattern "$edge1_log" '\[edge\] registered to' 10; then
  show_file_tail "$edge1_log"
  die "edge1 did not finish registration"
fi

# Edge2 기동
"$binary" "$MQTT_HOST" "$MQTT_PORT" "$MQTT_HOST" "$MQTT_PORT" \
  >"$edge2_log" 2>&1 &
register_pid "$!"

if ! wait_for_pattern "$edge2_log" '\[edge\] registered to' 10; then
  show_file_tail "$edge2_log"
  die "edge2 did not finish registration"
fi

# Core가 CT를 두 Edge 모두에게 브로드캐스트할 때까지 대기 (최대 10초)
if ! wait_for_pattern "$edge1_log" 'CT applied' 10; then
  show_file_tail "$edge1_log"
  die "edge1 did not receive CT"
fi
if ! wait_for_pattern "$edge2_log" 'CT applied' 10; then
  show_file_tail "$edge2_log"
  die "edge2 did not receive CT"
fi

# 각 Edge가 상대방에게 Ping을 발송했는지 확인
if ! wait_for_pattern "$edge1_log" 'ping sent to' 10; then
  show_file_tail "$edge1_log"
  die "edge1 did not send ping"
fi
if ! wait_for_pattern "$edge2_log" 'ping sent to' 10; then
  show_file_tail "$edge2_log"
  die "edge2 did not send ping"
fi

# 각 Edge가 Pong을 수신하고 RTT를 계산했는지 확인
if ! wait_for_pattern "$edge1_log" 'pong from .* RTT=' 10; then
  show_file_tail "$edge1_log"
  die "edge1 did not receive pong / calculate RTT"
fi
if ! wait_for_pattern "$edge2_log" 'pong from .* RTT=' 10; then
  show_file_tail "$edge2_log"
  die "edge2 did not receive pong / calculate RTT"
fi

# Relay Node 선택 로그 확인
if ! wait_for_pattern "$edge1_log" 'relay node selected:' 10; then
  show_file_tail "$edge1_log"
  die "edge1 did not select relay node"
fi
if ! wait_for_pattern "$edge2_log" 'relay node selected:' 10; then
  show_file_tail "$edge2_log"
  die "edge2 did not select relay node"
fi

log "RTT measurement and relay node selection ok"
log "logs kept in $TEST_RUN_DIR until script exit"
