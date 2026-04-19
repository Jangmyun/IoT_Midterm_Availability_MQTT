#!/usr/bin/env bash
# EDGE-08: Peer Relay 검증
# EdgeA(Core 직접) + EdgeC(EdgeA 경유) 토폴로지에서
# EdgeC → EdgeA → Core 이벤트 전달 경로 확인
#
# 토폴로지:
#   Core:MQTT_PORT
#     ↑
#   EdgeA (local_broker:2884) → upstream CORE → Core
#     ↑
#   EdgeC (local_broker:3884) → upstream PEER_EDGE → EdgeA local_broker:2884
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_cmd mosquitto mosquitto_pub mosquitto_sub
make_run_dir "edge-peer-relay" >/dev/null
setup_cleanup_trap

core_log="$TEST_RUN_DIR/core.log"
edge_a_log="$TEST_RUN_DIR/edge_a.log"
edge_c_log="$TEST_RUN_DIR/edge_c.log"
broker_a_log="$TEST_RUN_DIR/broker_a.log"
broker_c_log="$TEST_RUN_DIR/broker_c.log"

binary="$(edge_binary)"
edge_a_local_port="${EDGE_A_LOCAL_PORT:-2884}"
edge_c_local_port="${EDGE_C_LOCAL_PORT:-3884}"

# retained 메시지 클리어
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "campus/monitor/topology" -n -r 2>/dev/null || true
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "_core/sync/connection_table" -n -r 2>/dev/null || true
sleep 0.3

# Core 기동
start_core "$core_log" >/dev/null
if ! wait_for_pattern "$core_log" '\[core\] connected' 10; then
  show_file_tail "$core_log"
  die "core did not connect to broker"
fi

# EdgeA 전용 로컬 브로커 기동
mosquitto -p "$edge_a_local_port" >"$broker_a_log" 2>&1 &
register_pid "$!"
# EdgeC 전용 로컬 브로커 기동
mosquitto -p "$edge_c_local_port" >"$broker_c_log" 2>&1 &
register_pid "$!"
sleep 0.5

# TC-00: EdgeA 기동 (Core 직접 연결)
EDGE_ID_SUFFIX="-peerA" \
  "$binary" "$MQTT_HOST" "$edge_a_local_port" "$MQTT_HOST" "$MQTT_PORT" \
  >"$edge_a_log" 2>&1 &
register_pid "$!"

if ! wait_for_pattern "$edge_a_log" '\[edge\] registered to core' 10; then
  show_file_tail "$edge_a_log"
  die "EdgeA did not register to Core"
fi

# EdgeA가 CT를 수신하고 로컬 브로커에 retain 재발행할 시간 대기
sleep 1

# TC-00: EdgeC 기동 (peer-only, EdgeA 로컬 브로커 경유)
EDGE_PEER_ONLY=1 \
EDGE_UPSTREAM_PEERS="127.0.0.1:${edge_a_local_port}" \
EDGE_ID_SUFFIX="-peerC" \
  "$binary" "$MQTT_HOST" "$edge_c_local_port" "$MQTT_HOST" "$MQTT_PORT" \
  >"$edge_c_log" 2>&1 &
register_pid "$!"

if ! wait_for_pattern "$edge_c_log" '\[edge\] connected to peer edge' 10; then
  show_file_tail "$edge_c_log"
  die "EdgeC did not connect to peer edge (EdgeA local broker)"
fi

# TC-01: EdgeC가 EdgeA 로컬 브로커를 통해 CT를 수신
log "TC-01: EdgeC receives CT via EdgeA local broker"
if ! wait_for_pattern "$edge_c_log" 'CT applied' 15; then
  show_file_tail "$edge_c_log"
  die "TC-01 FAIL: EdgeC did not receive CT via peer"
fi
log "TC-01 PASS"

# TC-02: EdgeA가 EdgeC의 status를 Core로 전달
log "TC-02: EdgeA forwards EdgeC status to Core"
if ! wait_for_pattern "$edge_a_log" 'forwarded peer status' 10; then
  show_file_tail "$edge_a_log"
  die "TC-02 FAIL: EdgeA did not forward EdgeC status to Core"
fi
log "TC-02 PASS"

# TC-03: EdgeC에서 발행한 이벤트가 EdgeA를 거쳐 Core에 도달
log "TC-03: Event from EdgeC reaches Core via EdgeA"
recv_log="$TEST_RUN_DIR/recv.log"

# Core 브로커에서 campus/data/# 구독 (메시지 1개 수신 후 자동 종료, 15초 타임아웃)
mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" -t 'campus/data/#' \
  -C 1 -W 15 >"$recv_log" 2>&1 &
sub_pid="$!"
register_pid "$sub_pid"
sleep 0.3

# EdgeC 로컬 브로커에 이벤트 발행 → EdgeC → EdgeA → Core 경유
mosquitto_pub -h "$MQTT_HOST" -p "$edge_c_local_port" \
  -t "campus/data/motion/B1/CAM-PEER" -m '{"type":"MOTION","priority":"LOW"}'

# mosquitto_sub -C 1 -W 15가 성공(rc=0)으로 종료되면 Core에 도달한 것
if ! wait "$sub_pid"; then
  show_file_tail "$recv_log"
  die "TC-03 FAIL: Event from EdgeC did not reach Core broker"
fi
log "TC-03 PASS: event received at Core broker"

log "all peer-relay tests passed"
log "logs kept in $TEST_RUN_DIR"
