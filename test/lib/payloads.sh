#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "This file must be sourced, not executed." >&2
  exit 1
fi

priority_line() {
  local priority="${1:-}"

  if [[ -n "$priority" ]]; then
    printf '  "priority": "%s",\n' "$priority"
  fi
}

emit_mqtt_message_json() {
  local msg_id="$1"
  local type="$2"
  local source_role="$3"
  local source_id="$4"
  local target_role="$5"
  local target_id="$6"
  local original_node="$7"
  local prev_hop="$8"
  local next_hop="$9"
  local hop_count="${10}"
  local ttl="${11}"
  local qos="${12}"
  local dup="${13}"
  local retain="${14}"
  local building_id="${15}"
  local camera_id="${16}"
  local description="${17}"
  local priority="${18:-}"

  cat <<EOF
{
  "msg_id": "$msg_id",
  "type": "$type",
  "timestamp": "$(iso_timestamp)",
$(priority_line "$priority")
  "source": {
    "role": "$source_role",
    "id": "$source_id"
  },
  "target": {
    "role": "$target_role",
    "id": "$target_id"
  },
  "route": {
    "original_node": "$original_node",
    "prev_hop": "$prev_hop",
    "next_hop": "$next_hop",
    "hop_count": $hop_count,
    "ttl": $ttl
  },
  "delivery": {
    "qos": $qos,
    "dup": $dup,
    "retain": $retain
  },
  "payload": {
    "building_id": "$building_id",
    "camera_id": "$camera_id",
    "description": "$description"
  }
}
EOF
}

emit_topology_json() {
  local version="${1:-1}"
  local active_core_id="${2:-$CORE_A_ID}"
  local backup_core_id="${3:-$CORE_B_ID}"
  local node_1_status="${4:-$NODE_1_STATUS}"
  local node_2_status="${5:-$NODE_2_STATUS}"

  cat <<EOF
{
  "version": $version,
  "last_update": "$(iso_timestamp)",
  "active_core_id": "$active_core_id",
  "backup_core_id": "$backup_core_id",
  "node_count": 4,
  "nodes": [
    {"id":"$CORE_A_ID","role":"CORE","ip":"$CORE_A_IP","port":$CORE_A_PORT,"status":"ONLINE","hop_to_core":0},
    {"id":"$CORE_B_ID","role":"CORE","ip":"$CORE_B_IP","port":$CORE_B_PORT,"status":"ONLINE","hop_to_core":1},
    {"id":"$NODE_1_ID","role":"NODE","ip":"$NODE_1_IP","port":$NODE_1_PORT,"status":"$node_1_status","hop_to_core":$NODE_1_HOP},
    {"id":"$NODE_2_ID","role":"NODE","ip":"$NODE_2_IP","port":$NODE_2_PORT,"status":"$node_2_status","hop_to_core":$NODE_2_HOP}
  ],
  "link_count": 3,
  "links": [
    {"from_id":"$CORE_A_ID","to_id":"$CORE_B_ID","rtt_ms":1.2},
    {"from_id":"$CORE_A_ID","to_id":"$NODE_1_ID","rtt_ms":4.7},
    {"from_id":"$CORE_A_ID","to_id":"$NODE_2_ID","rtt_ms":8.1}
  ]
}
EOF
}

emit_event_json() {
  local type="$1"
  local priority="$2"
  local source_id="$3"
  local target_id="$4"
  local building_id="$5"
  local camera_id="$6"
  local description="$7"
  local msg_id="${8:-$(gen_uuid)}"

  emit_mqtt_message_json \
    "$msg_id" "$type" "NODE" "$source_id" "CORE" "$target_id" \
    "$source_id" "$source_id" "$target_id" \
    1 5 1 false false \
    "$building_id" "$camera_id" "$description" "$priority"
}

emit_status_json() {
  local source_role="$1"
  local source_id="$2"
  local target_role="$3"
  local target_id="$4"
  local description="$5"
  local building_id="${6:-}"
  local camera_id="${7:-}"
  local msg_id="${8:-$(gen_uuid)}"
  local next_hop="${9:-$target_id}"
  local hop_count="${10:-0}"
  local ttl="${11:-1}"

  emit_mqtt_message_json \
    "$msg_id" "STATUS" "$source_role" "$source_id" "$target_role" "$target_id" \
    "$source_id" "$source_id" "$next_hop" \
    "$hop_count" "$ttl" 1 false false \
    "$building_id" "$camera_id" "$description"
}

emit_core_lwt_notice_json() {
  local source_id="$1"
  local description="$2"
  local msg_id="${3:-$(gen_uuid)}"

  emit_mqtt_message_json \
    "$msg_id" "LWT_CORE" "CORE" "$source_id" "NODE" "all" \
    "$source_id" "$source_id" "all" \
    0 1 1 false false \
    "" "" "$description"
}

emit_core_lwt_payload_json() {
  local source_id="$1"
  local backup_core_id="$2"
  local backup_ip="$3"
  local backup_port="$4"
  local msg_id="${5:-$source_id}"

  emit_mqtt_message_json \
    "$msg_id" "LWT_CORE" "CORE" "$source_id" "CORE" "$backup_core_id" \
    "$source_id" "$source_id" "$backup_core_id" \
    0 1 1 false false \
    "" "" "$backup_ip:$backup_port"
}

emit_ping_json() {
  local requester_id="$1"
  local target_id="$2"
  local msg_id="${3:-$(gen_uuid)}"

  emit_mqtt_message_json \
    "$msg_id" "PING_REQ" "NODE" "$requester_id" "NODE" "$target_id" \
    "$requester_id" "$requester_id" "$target_id" \
    1 3 0 false false \
    "" "" "RTT probe"
}
