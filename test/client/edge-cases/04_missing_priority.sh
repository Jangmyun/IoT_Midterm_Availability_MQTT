#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_event_json "MOTION" "" "$NODE_2_ID" "$CORE_A_ID" "bldg-b" "cam-08" "Priority omitted test")"
mqtt_publish_json "campus/data/MOTION/bldg-b" 1 false "$payload"

log "published event without priority"
log "expected: client accepts the message and treats priority as null"
