#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../../lib/common.sh"
source "$SCRIPT_DIR/../../lib/payloads.sh"

require_cmd mosquitto_pub

payload="$(emit_topology_json "${TOPOLOGY_VERSION:-1}")"
mqtt_publish_json "campus/monitor/topology" 1 true "$payload"

log "published topology to $MQTT_HOST:$MQTT_PORT (retain=true, version=${TOPOLOGY_VERSION:-1})"
