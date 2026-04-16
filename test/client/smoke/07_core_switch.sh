#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_status_json "CORE" "$CORE_B_ID" "NODE" "all" "Active Core switched to CORE_B" "" "" "" "all")"
mqtt_publish_json "campus/alert/core_switch" 1 false "$payload"

log "published core_switch alert"
