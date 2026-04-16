#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

"$SCRIPT_DIR/01_topology.sh"
sleep 1
"$SCRIPT_DIR/02_event_intrusion.sh"
sleep 1
"$SCRIPT_DIR/03_event_motion.sh"
sleep 1
"$SCRIPT_DIR/04_event_door_forced.sh"
sleep 1
"$SCRIPT_DIR/05_node_down.sh"
sleep 1
"$SCRIPT_DIR/06_node_up.sh"
sleep 1
"$SCRIPT_DIR/07_core_switch.sh"
sleep 1
"$SCRIPT_DIR/08_core_lwt.sh"

printf '[test] smoke sequence completed\n'
