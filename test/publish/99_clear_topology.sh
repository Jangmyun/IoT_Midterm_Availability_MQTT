#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_cmd mosquitto_pub

mqtt_clear_retained "campus/monitor/topology"

log "cleared retained topology"
