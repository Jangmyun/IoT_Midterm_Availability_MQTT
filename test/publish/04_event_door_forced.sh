#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_event_json "DOOR_FORCED" "HIGH" "$NODE_2_ID" "$CORE_A_ID" "bldg-c" "door-02" "Forced door event at west entrance")"
mqtt_publish_json "campus/data/DOOR_FORCED/bldg-c" 1 false "$payload"

log "published DOOR_FORCED event"
