#!/usr/bin/env bash
# EDGE-04: CT active_core_id 변경 감지 → Edge 새 Active Core 재연결 검증 (FR-05, FR-10)
# 초기 CT(core_a가 active) 수신 후, core_b가 active인 CT를 수신하면
# Edge가 core_b IP:Port로 재연결을 시도하는지 확인
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "edge-ct-active-core-change" >/dev/null
setup_cleanup_trap

edge_log="$TEST_RUN_DIR/edge.log"

CORE_A_UUID="$(gen_uuid)"
CORE_B_UUID="$(gen_uuid)"
NEW_CORE_IP="$MQTT_HOST"
NEW_CORE_PORT=19883   # 실제로는 없는 포트 — 재연결 시도 로그만 확인

# 잔류 retained CT보다 높은 버전에서 시작 (epoch 기반 오프셋)
BASE_VERSION=$(( ($(date +%s) % 100000) + 50000 ))

# CT JSON 생성 헬퍼
make_ct_json() {
  local version="$1"
  local active_core_id="$2"
  local core_uuid="$3"
  local core_ip="$4"
  local core_port="$5"

  cat <<EOF
{
  "version": $version,
  "last_update": "$(iso_timestamp)Z",
  "active_core_id": "$active_core_id",
  "backup_core_id": "",
  "nodes": [
    {
      "id": "$core_uuid",
      "role": "CORE",
      "ip": "$core_ip",
      "port": $core_port,
      "status": "ONLINE",
      "hop_to_core": 0
    }
  ],
  "links": []
}
EOF
}

# 잔류 retained 토픽 클리어 (이전 테스트 실행의 오염된 CT 제거)
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "campus/monitor/topology" -n -r 2>/dev/null || true
mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "_core/sync/connection_table" -n -r 2>/dev/null || true
sleep 0.3

# Edge만 기동 (Core 없이 직접 CT 주입)
start_edge "$edge_log" >/dev/null
if ! wait_for_pattern "$edge_log" '\[edge\] connected to core' 10; then
  show_file_tail "$edge_log"
  die "edge did not connect to broker"
fi
sleep 0.5

CT_V1=$(( BASE_VERSION ))
CT_V2=$(( BASE_VERSION + 1 ))

# CT v1: CORE_A가 active (현재 연결 중인 MQTT_HOST:MQTT_PORT)
ct1="$(make_ct_json $CT_V1 "$CORE_A_UUID" "$CORE_A_UUID" "$MQTT_HOST" "$MQTT_PORT")"
mqtt_publish_json "campus/monitor/topology" 1 true "$ct1"

if ! wait_for_pattern "$edge_log" "CT applied \(version=${CT_V1}" 10; then
  show_file_tail "$edge_log"
  die "edge did not apply CT v1"
fi
sleep 0.3

# CT v2: CORE_B가 active, CORE_B는 NEW_CORE_IP:NEW_CORE_PORT에 있음
ct2="$(make_ct_json $CT_V2 "$CORE_B_UUID" "$CORE_B_UUID" "$NEW_CORE_IP" "$NEW_CORE_PORT")"
mqtt_publish_json "campus/monitor/topology" 1 true "$ct2"

if ! wait_for_pattern "$edge_log" "CT applied \(version=${CT_V2}" 10; then
  show_file_tail "$edge_log"
  die "edge did not apply CT v2"
fi

# active_core_id 변경 감지 후 재연결 시도 확인
if ! wait_for_pattern "$edge_log" \
  "active_core_id changed.*reconnect to ${NEW_CORE_IP}:${NEW_CORE_PORT}" 10; then
  show_file_tail "$edge_log"
  die "edge did not detect active_core_id change and attempt reconnect"
fi

log "CT active_core_id change ok: edge reconnecting to ${NEW_CORE_IP}:${NEW_CORE_PORT}"
log "logs kept in $TEST_RUN_DIR until script exit"
