#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_event_json "INTRUSION" "HIGH" "$NODE_1_ID" "$CORE_A_ID" "bldg-a" "cam-01" "Intrusion detected at main gate CCTV")"
mqtt_publish_json "campus/data/INTRUSION" 1 false "$payload"

log "published INTRUSION event"
