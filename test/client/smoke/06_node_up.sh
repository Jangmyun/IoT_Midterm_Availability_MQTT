#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

# core가 node_up 알림 시 최신 ConnectionTable을 payload로 보낸다고 가정 (node_down과 동일 구조)
payload="$(emit_topology_json 3 "$CORE_A_ID" "$CORE_B_ID" "ONLINE" "ONLINE")"
mqtt_publish_json "campus/alert/node_up/$NODE_1_ID" 1 false "$payload"

log "published node_up alert (CT payload: NODE_1=$NODE_1_ID ONLINE, ct.version=3)"
