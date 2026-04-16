#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto_pub

msg_id="$(gen_uuid)"
payload="$(emit_event_json "INTRUSION" "HIGH" "$NODE_1_ID" "$CORE_A_ID" "bldg-a" "cam-01" "Duplicate event test" "$msg_id")"

mqtt_publish_json "campus/data/INTRUSION" 1 false "$payload"
sleep 1
mqtt_publish_json "campus/data/INTRUSION" 1 false "$payload"

log "published duplicate event twice with msg_id=$msg_id"
log "expected: client deduplicates by msg_id and shows a single log entry"
