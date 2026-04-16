#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

require_cmd mosquitto_pub

payload="$(cat <<EOF
{
  "msg_id": "not-a-uuid",
  "type": "INTRUSION",
  "timestamp": "$(iso_timestamp)",
  "source": {"role": "NODE", "id": "$NODE_1_ID"},
  "target": {"role": "CORE", "id": "$CORE_A_ID"},
  "route": {"original_node": "$NODE_1_ID", "prev_hop": "$NODE_1_ID", "next_hop": "$CORE_A_ID", "hop_count": 1, "ttl": 5},
  "delivery": {"qos": 1, "dup": false, "retain": false},
  "payload": {"building_id": "bldg-a", "camera_id": "cam-01", "description": "Invalid UUID should be ignored"}
}
EOF
)"

mqtt_publish_json "campus/data/INTRUSION" 1 false "$payload"

log "published event with invalid msg_id"
log "expected: client parser drops the message"
