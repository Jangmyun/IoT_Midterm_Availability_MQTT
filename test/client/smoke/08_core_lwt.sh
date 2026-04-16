#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_core_lwt_notice_json "$CORE_A_ID" "Core A terminated unexpectedly; backup at $CORE_B_IP:$CORE_B_PORT")"
mqtt_publish_json "campus/will/core/$CORE_A_ID" 1 false "$payload"

log "published synthetic core LWT notice"
