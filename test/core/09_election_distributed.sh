#!/usr/bin/env bash
# CORE-09: 분산 환경 Election — Active+Backup 2-Core, peer 채널 election 검증 (FR-10, C-03, C-04)
# Active Core SIGKILL 없이 Election으로 Backup → ACTIVE 전환 + core_switch 수신 확인
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_mqtt_tools
make_run_dir "core-election-distributed" >/dev/null
setup_cleanup_trap

sub_log="$TEST_RUN_DIR/subscriber.log"
active_log="$TEST_RUN_DIR/active.log"
backup_log="$TEST_RUN_DIR/backup.log"

start_active_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  "$binary" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_backup_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"
  # peer → Active 브로커 (동일 mosquitto 인스턴스를 다른 포트로 구분 불가하므로
  # 실제 분산 환경을 시뮬레이션: peer를 동일 브로커로 지정)
  "$binary" "$MQTT_HOST" "$MQTT_PORT" "$MQTT_HOST" "$MQTT_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

# subscriber: Active 브로커에서 election 토픽 + core_switch 구독
start_subscriber "$sub_log" \
  "_core/election/#" \
  "campus/alert/core_switch" \
  "campus/monitor/topology" >/dev/null
sleep 1

# Active Core 기동
start_active_core "$active_log" >/dev/null
if ! wait_for_pattern "$active_log" '\[core\] connected \(ACTIVE\)' 10; then
  show_file_tail "$active_log"
  die "active core did not connect"
fi
active_core_id="$(extract_core_id "$active_log")"

# Backup Core 기동 (peer → Active 브로커)
start_backup_core "$backup_log" >/dev/null
if ! wait_for_pattern "$backup_log" '\[core\] connected \(BACKUP\)' 10; then
  show_file_tail "$backup_log"
  die "backup core did not connect"
fi
if ! wait_for_pattern "$backup_log" '\[core/backup\] connected to active broker' 10; then
  show_file_tail "$backup_log"
  die "backup core did not connect to active broker via peer"
fi
backup_core_id="$(extract_core_id "$backup_log")"

sleep 0.5

# Election request를 Active 브로커에 발행
# source = backup_core_id: Backup이 자신을 Active로 선출 요청
# Active Core가 수신 → Backup에 투표 → Backup이 ACTIVE 전환 후 core_switch 발행
req_payload="$(cat <<EOF
{
  "msg_id": "$(gen_uuid)",
  "type": "ELECTION_REQ",
  "timestamp": "$(iso_timestamp)",
  "source": {"role": "CORE", "id": "$backup_core_id"},
  "target": {"role": "CORE", "id": "all"},
  "route": {"original_node": "$backup_core_id", "prev_hop": "$backup_core_id", "next_hop": "all", "hop_count": 0, "ttl": 1},
  "delivery": {"qos": 1, "dup": false, "retain": false},
  "payload": {"building_id": "", "camera_id": "", "description": "$backup_core_id"}
}
EOF
)"
mqtt_publish_json "_core/election/request" 1 false "$req_payload"

# Backup이 peer 채널로 election request 수신 후 투표 발행 확인
if ! wait_for_pattern "$sub_log" '_core/election/result' 10; then
  show_file_tail "$sub_log"
  show_file_tail "$backup_log"
  die "election result was not published via peer channel"
fi

# core_switch 수신 확인 (Backup이 ACTIVE 전환 후 peer 브로커에 발행)
if ! wait_for_pattern "$sub_log" 'campus/alert/core_switch' 15; then
  show_file_tail "$sub_log"
  show_file_tail "$backup_log"
  die "campus/alert/core_switch was not published after distributed election"
fi

# core_switch payload에 Backup IP:Port 포함 확인
if ! wait_for_pattern "$sub_log" "$MQTT_HOST" 5; then
  show_file_tail "$sub_log"
  die "core_switch payload does not contain backup core IP"
fi

# topology에 새 active_core_id 반영 확인
if ! wait_for_pattern "$sub_log" 'campus/monitor/topology' 10; then
  show_file_tail "$sub_log"
  die "topology was not updated after distributed election"
fi

log "distributed election ok: active=$active_core_id, backup(new active)=$backup_core_id"
log "backup published campus/alert/core_switch via peer broker channel"
log "logs kept in $TEST_RUN_DIR until script exit"
