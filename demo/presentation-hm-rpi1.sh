#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${DEMO_BUILD_DIR:-$ROOT_DIR/broker/build}"

ACTIVE_HOST="${DEMO_ACTIVE_CORE_HOST:-192.168.0.7}"
ACTIVE_PORT="${DEMO_ACTIVE_CORE_PORT:-1883}"
BACKUP_HOST="${DEMO_BACKUP_CORE_HOST:-192.168.0.8}"
BACKUP_PORT="${DEMO_BACKUP_CORE_PORT:-1883}"

info() {
  printf '[demo] %s\n' "$*"
}

warn() {
  printf '[demo][warn] %s\n' "$*" >&2
}

die() {
  printf '[demo][error] %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<EOF
Usage:
  ./demo/presentation-hm-rpi1.sh show
  ./demo/presentation-hm-rpi1.sh active-core [self_host] [self_port]
  ./demo/presentation-hm-rpi1.sh backup-core [self_host] [self_port]
  ./demo/presentation-hm-rpi1.sh active-sub
  ./demo/presentation-hm-rpi1.sh backup-sub
  ./demo/presentation-hm-rpi1.sh edge <local_host> <local_port>

Fixed presentation topology:
  Active Core : $ACTIVE_HOST:$ACTIVE_PORT
  Backup Core : $BACKUP_HOST:$BACKUP_PORT

Examples:
  # On Hyeokmin Mac (192.168.0.7)
  ./demo/presentation-hm-rpi1.sh active-core

  # On Raspberry Pi 1 (192.168.0.8)
  ./demo/presentation-hm-rpi1.sh backup-core

  # If Raspberry Pi 1 currently uses another IP
  ./demo/presentation-hm-rpi1.sh backup-core 192.168.0.23

  # On any edge node
  ./demo/presentation-hm-rpi1.sh edge 192.168.0.9 2883
EOF
}

require_binary() {
  local path="$1"
  [[ -x "$path" ]] || die "missing executable: $path"
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

list_local_ips() {
  if command -v ifconfig >/dev/null 2>&1; then
    ifconfig 2>/dev/null | awk '/inet /{print $2}' | grep -v '^127\.' || true
    return
  fi

  if command -v hostname >/dev/null 2>&1; then
    hostname -I 2>/dev/null | tr ' ' '\n' | sed '/^$/d' || true
  fi
}

assert_current_host_matches() {
  local expected_ip="$1"

  if list_local_ips | grep -Fx "$expected_ip" >/dev/null 2>&1; then
    return 0
  fi

  warn "current machine does not appear to own $expected_ip"
  warn "local IPs detected: $(list_local_ips | tr '\n' ' ' | sed 's/ $//')"
}

show_topology() {
  cat <<EOF
Presentation topology
  Active Core : $ACTIVE_HOST:$ACTIVE_PORT
  Backup Core : $BACKUP_HOST:$BACKUP_PORT

One-command startup
  Mac ($ACTIVE_HOST):
    ./demo/presentation-hm-rpi1.sh active-core

  Raspberry Pi 1 ($BACKUP_HOST by default):
    ./demo/presentation-hm-rpi1.sh backup-core
    ./demo/presentation-hm-rpi1.sh backup-core <actual_rpi_ip>

Failover 후 원래 Active를 Backup으로 재진입:
  ./demo/presentation-hm-rpi1.sh rejoin-as-backup
  ./demo/presentation-hm-rpi1.sh rejoin-as-backup [new_active_host] [new_active_port]

  self는 항상 ${ACTIVE_HOST}:${ACTIVE_PORT}로 고정, 첫 인수가 peer(새 Active) 주소
  기본값: new_active=${BACKUP_HOST}:${BACKUP_PORT}
  예) Active(192.168.0.7)가 죽고 Backup(192.168.0.8)이 승격된 경우:
    ./demo/presentation-hm-rpi1.sh rejoin-as-backup
    ./demo/presentation-hm-rpi1.sh rejoin-as-backup 192.168.0.8 1883  # 동일

Typical edge command
  ./demo/presentation-hm-rpi1.sh edge <local_host> <local_port>
EOF
}

run_active_core() {
  local self_host="${1:-$ACTIVE_HOST}"
  local self_port="${2:-$ACTIVE_PORT}"
  assert_current_host_matches "$self_host"
  require_binary "$BUILD_DIR/core_broker"
  info "starting active core at $self_host:$self_port"
  exec "$BUILD_DIR/core_broker" "$self_host" "$self_port"
}

run_backup_core() {
  local self_host="${1:-$BACKUP_HOST}"
  local self_port="${2:-$BACKUP_PORT}"
  assert_current_host_matches "$self_host"
  require_binary "$BUILD_DIR/core_broker"
  info "starting backup core at $self_host:$self_port (peer=$ACTIVE_HOST:$ACTIVE_PORT)"
  exec "$BUILD_DIR/core_broker" \
    "$self_host" "$self_port" \
    "$ACTIVE_HOST" "$ACTIVE_PORT"
}

run_rejoin_as_backup() {
  # 원래 Active(ACTIVE_HOST)가 새 Active(승격된 Backup)에 peer 연결하여 Backup으로 재진입.
  # self는 항상 $ACTIVE_HOST:$ACTIVE_PORT — 첫 번째 인수는 new_active_host (peer 주소).
  local self_host="$ACTIVE_HOST"
  local self_port="$ACTIVE_PORT"
  local new_active_host="${1:-$BACKUP_HOST}"
  local new_active_port="${2:-$BACKUP_PORT}"
  assert_current_host_matches "$self_host"
  require_binary "$BUILD_DIR/core_broker"
  info "rejoining as backup: self=$self_host:$self_port  new_active=$new_active_host:$new_active_port"
  exec "$BUILD_DIR/core_broker" \
    "$self_host" "$self_port" \
    "$new_active_host" "$new_active_port"
}

run_active_sub() {
  require_cmd mosquitto_sub
  exec mosquitto_sub -h "$ACTIVE_HOST" -p "$ACTIVE_PORT" -v \
    -t 'campus/#' \
    -t '_core/#'
}

run_backup_sub() {
  require_cmd mosquitto_sub
  exec mosquitto_sub -h "$BACKUP_HOST" -p "$BACKUP_PORT" -v \
    -t 'campus/#' \
    -t '_core/#'
}

run_edge() {
  local local_host="$1"
  local local_port="$2"

  require_binary "$BUILD_DIR/edge_broker"
  info "starting edge local=$local_host:$local_port core=$ACTIVE_HOST:$ACTIVE_PORT backup=$BACKUP_HOST:$BACKUP_PORT"
  exec "$BUILD_DIR/edge_broker" \
    "$local_host" "$local_port" \
    "$ACTIVE_HOST" "$ACTIVE_PORT" \
    "$BACKUP_HOST" "$BACKUP_PORT"
}

case "${1:-show}" in
  show)
    show_topology
    ;;
  active-core)
    run_active_core "${2:-}" "${3:-}"
    ;;
  backup-core)
    run_backup_core "${2:-}" "${3:-}"
    ;;
  rejoin-as-backup)
    run_rejoin_as_backup "${2:-}" "${3:-}"
    ;;
  active-sub)
    run_active_sub
    ;;
  backup-sub)
    run_backup_sub
    ;;
  edge)
    [[ $# -eq 3 ]] || die "usage: ./demo/presentation-hm-rpi1.sh edge <local_host> <local_port>"
    run_edge "$2" "$3"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
