#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_status_json "CORE" "$CORE_A_ID" "NODE" "$NODE_1_ID" "Node offline" "bldg-a" "" "" "$NODE_1_ID")"
mqtt_publish_json "campus/alert/node_down/$NODE_1_ID" 1 false "$payload"

log "published node_down alert"
