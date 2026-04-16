#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/payloads.sh"

require_cmd mosquitto_pub

new_payload="$(emit_topology_json "${NEW_VERSION:-10}" "$CORE_A_ID" "$CORE_B_ID" "ONLINE" "ONLINE")"
old_payload="$(emit_topology_json "${OLD_VERSION:-9}" "$CORE_A_ID" "$CORE_B_ID" "OFFLINE" "ONLINE")"

mqtt_publish_json "campus/monitor/topology" 1 true "$new_payload"
sleep 1
mqtt_publish_json "campus/monitor/topology" 1 true "$old_payload"

log "published topology version ${NEW_VERSION:-10}, then stale version ${OLD_VERSION:-9}"
log "expected: connected client ignores the second topology because version decreased"
