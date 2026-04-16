#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
  cat <<'EOF'
Usage: ./test/test_pub.sh <case>

Publish smoke cases:
  topology
  event_intrusion
  event_motion
  event_door_forced
  node_down
  node_up
  core_switch
  lwt
  all
  clear_topology

Edge-case publish cases:
  duplicate_event
  stale_topology
  invalid_uuid
  missing_priority

Integration cases:
  integration_bootstrap
  integration_ping_pong
  integration_core_lwt
  integration_edge_lwt

Meta:
  list
  help

Common env:
  MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD
  CORE_A_IP, CORE_B_IP, NODE_1_IP, NODE_2_IP
  BUILD_DIR, CORE_BINARY, EDGE_BINARY, EDGE_NODE_PORT
EOF
}

list_cases() {
  cat <<'EOF'
publish/topology
publish/event_intrusion
publish/event_motion
publish/event_door_forced
publish/node_down
publish/node_up
publish/core_switch
publish/lwt
publish/all
publish/clear_topology
edge-cases/duplicate_event
edge-cases/stale_topology
edge-cases/invalid_uuid
edge-cases/missing_priority
integration/bootstrap
integration/ping_pong
integration/core_lwt
integration/edge_lwt
EOF
}

case "${1:-help}" in
  topology)               exec "$SCRIPT_DIR/publish/01_topology.sh" ;;
  event_intrusion)        exec "$SCRIPT_DIR/publish/02_event_intrusion.sh" ;;
  event_motion)           exec "$SCRIPT_DIR/publish/03_event_motion.sh" ;;
  event_door_forced)      exec "$SCRIPT_DIR/publish/04_event_door_forced.sh" ;;
  node_down)              exec "$SCRIPT_DIR/publish/05_node_down.sh" ;;
  node_up)                exec "$SCRIPT_DIR/publish/06_node_up.sh" ;;
  core_switch)            exec "$SCRIPT_DIR/publish/07_core_switch.sh" ;;
  lwt)                    exec "$SCRIPT_DIR/publish/08_core_lwt.sh" ;;
  all)                    exec "$SCRIPT_DIR/publish/90_all_smoke.sh" ;;
  clear_topology)         exec "$SCRIPT_DIR/publish/99_clear_topology.sh" ;;
  duplicate_event)        exec "$SCRIPT_DIR/edge-cases/01_duplicate_event.sh" ;;
  stale_topology)         exec "$SCRIPT_DIR/edge-cases/02_stale_topology.sh" ;;
  invalid_uuid)           exec "$SCRIPT_DIR/edge-cases/03_invalid_event_uuid.sh" ;;
  missing_priority)       exec "$SCRIPT_DIR/edge-cases/04_missing_priority.sh" ;;
  integration_bootstrap)  exec "$SCRIPT_DIR/integration/01_core_edge_bootstrap.sh" ;;
  integration_ping_pong)  exec "$SCRIPT_DIR/integration/02_edge_ping_pong.sh" ;;
  integration_core_lwt)   exec "$SCRIPT_DIR/integration/03_core_lwt_payload.sh" ;;
  integration_edge_lwt)   exec "$SCRIPT_DIR/integration/04_edge_lwt_emission.sh" ;;
  list)                   list_cases ;;
  help|-h|--help)         usage ;;
  *)
    usage >&2
    exit 1
    ;;
esac
