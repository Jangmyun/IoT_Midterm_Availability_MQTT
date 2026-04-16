#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

# core/main.cpp 는 node_down 토픽에 ConnectionTable JSON을 publish함 (version guard 포함)
payload="$(emit_topology_json 2 "$CORE_A_ID" "$CORE_B_ID" "OFFLINE" "ONLINE")"
mqtt_publish_json "campus/alert/node_down/$NODE_1_ID" 1 false "$payload"

log "published node_down alert (CT payload: NODE_1=$NODE_1_ID OFFLINE, ct.version=2)"
