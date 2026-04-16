#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "This file must be sourced, not executed." >&2
  exit 1
fi

set -euo pipefail

TEST_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_ROOT="$(cd "$TEST_LIB_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$TEST_ROOT/.." && pwd)"

: "${MQTT_HOST:=127.0.0.1}"
: "${MQTT_PORT:=1883}"
: "${MQTT_USERNAME:=}"
: "${MQTT_PASSWORD:=}"
: "${MQTT_TIMEOUT:=10}"
: "${BUILD_DIR:=$PROJECT_ROOT/build}"

: "${EDGE_BIND_HOST:=127.0.0.1}"
: "${EDGE_NODE_PORT:=${MQTT_PORT}}"
: "${EDGE_CORE_HOST:=$MQTT_HOST}"
: "${EDGE_CORE_PORT:=$MQTT_PORT}"
: "${EDGE_BACKUP_CORE_IP:=}"
: "${EDGE_BACKUP_CORE_PORT:=1883}"

: "${BACKUP_CORE_ID:=bbbbbbbb-0000-0000-0000-000000000002}"
: "${BACKUP_CORE_IP:=$MQTT_HOST}"
: "${BACKUP_CORE_PORT:=$MQTT_PORT}"

: "${CORE_A_ID:=aaaaaaaa-0000-0000-0000-000000000001}"
: "${CORE_B_ID:=bbbbbbbb-0000-0000-0000-000000000002}"
: "${NODE_1_ID:=cccccccc-0000-0000-0000-000000000003}"
: "${NODE_2_ID:=dddddddd-0000-0000-0000-000000000004}"
: "${PING_REQUESTER_ID:=eeeeeeee-0000-0000-0000-000000000005}"

: "${CORE_A_IP:=127.0.0.1}"
: "${CORE_B_IP:=127.0.0.2}"
: "${NODE_1_IP:=10.0.0.3}"
: "${NODE_2_IP:=10.0.0.4}"

: "${CORE_A_PORT:=1883}"
: "${CORE_B_PORT:=1883}"
: "${NODE_1_PORT:=1883}"
: "${NODE_2_PORT:=1883}"

: "${NODE_1_HOP:=2}"
: "${NODE_2_HOP:=2}"
: "${NODE_1_STATUS:=ONLINE}"
: "${NODE_2_STATUS:=ONLINE}"

TEST_RUN_DIR=""
TEST_PIDS=()

log() {
  printf '[test] %s\n' "$*"
}

die() {
  printf '[test][error] %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  local cmd
  for cmd in "$@"; do
    command -v "$cmd" >/dev/null 2>&1 || die "required command not found: $cmd"
  done
}

require_mqtt_tools() {
  require_cmd mosquitto_pub mosquitto_sub
}

iso_timestamp() {
  date -u +%Y-%m-%dT%H:%M:%S
}

gen_uuid() {
  if command -v uuidgen >/dev/null 2>&1; then
    uuidgen | tr '[:upper:]' '[:lower:]'
    return 0
  fi

  if [[ -r /proc/sys/kernel/random/uuid ]]; then
    tr '[:upper:]' '[:lower:]' < /proc/sys/kernel/random/uuid
    return 0
  fi

  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import uuid; print(uuid.uuid4())'
    return 0
  fi

  die "uuid generator not available (tried uuidgen, /proc, python3)"
}

make_run_dir() {
  local name="${1:-run}"
  TEST_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mqtt-test.${name}.XXXXXX")"
  printf '%s\n' "$TEST_RUN_DIR"
}

register_pid() {
  TEST_PIDS+=("$1")
}

cleanup() {
  local pid

  for pid in "${TEST_PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done

  for pid in "${TEST_PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done

  if [[ -n "${TEST_RUN_DIR:-}" && -d "$TEST_RUN_DIR" ]]; then
    rm -rf "$TEST_RUN_DIR"
  fi
}

setup_cleanup_trap() {
  trap cleanup EXIT INT TERM
}

show_file_tail() {
  local file="$1"
  local lines="${2:-40}"

  if [[ -f "$file" ]]; then
    printf '\n[test] tail -n %s %s\n' "$lines" "$file" >&2
    tail -n "$lines" "$file" >&2 || true
    printf '\n' >&2
  fi
}

wait_for_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout="${3:-$MQTT_TIMEOUT}"
  local elapsed=0

  while (( elapsed < timeout )); do
    if [[ -f "$file" ]] && grep -Eq "$pattern" "$file"; then
      return 0
    fi

    sleep 1
    elapsed=$((elapsed + 1))
  done

  return 1
}

mqtt_publish_json() {
  local topic="$1"
  local qos="$2"
  local retain="$3"
  local payload="$4"
  local cmd=(mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$topic" -q "$qos" -m "$payload")

  if [[ -n "$MQTT_USERNAME" ]]; then
    cmd+=(-u "$MQTT_USERNAME")
  fi
  if [[ -n "$MQTT_PASSWORD" ]]; then
    cmd+=(-P "$MQTT_PASSWORD")
  fi
  if [[ "$retain" == "true" ]]; then
    cmd+=(-r)
  fi

  "${cmd[@]}"
}

mqtt_clear_retained() {
  local topic="$1"
  local cmd=(mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "$topic" -n -r)

  if [[ -n "$MQTT_USERNAME" ]]; then
    cmd+=(-u "$MQTT_USERNAME")
  fi
  if [[ -n "$MQTT_PASSWORD" ]]; then
    cmd+=(-P "$MQTT_PASSWORD")
  fi

  "${cmd[@]}"
}

start_subscriber() {
  local outfile="$1"
  shift
  local cmd=(mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" -v)
  local topic

  if [[ -n "$MQTT_USERNAME" ]]; then
    cmd+=(-u "$MQTT_USERNAME")
  fi
  if [[ -n "$MQTT_PASSWORD" ]]; then
    cmd+=(-P "$MQTT_PASSWORD")
  fi

  for topic in "$@"; do
    cmd+=(-t "$topic")
  done

  "${cmd[@]}" >"$outfile" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

find_binary() {
  local name="$1"
  local candidate

  for candidate in \
    "${CORE_BINARY:-}" \
    "${EDGE_BINARY:-}" \
    "$BUILD_DIR/$name" \
    "$PROJECT_ROOT/build/$name" \
    "$PROJECT_ROOT/broker/build/$name" \
    "$PROJECT_ROOT/build/broker/$name"
  do
    if [[ -n "$candidate" && -x "$candidate" && "$(basename "$candidate")" == "$name" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

core_binary() {
  if [[ -n "${CORE_BINARY:-}" && -x "$CORE_BINARY" ]]; then
    printf '%s\n' "$CORE_BINARY"
    return 0
  fi

  find_binary core_broker || die "core_broker not found; set CORE_BINARY or BUILD_DIR"
}

edge_binary() {
  if [[ -n "${EDGE_BINARY:-}" && -x "$EDGE_BINARY" ]]; then
    printf '%s\n' "$EDGE_BINARY"
    return 0
  fi

  find_binary edge_broker || die "edge_broker not found; set EDGE_BINARY or BUILD_DIR"
}

start_core() {
  local log_file="$1"
  local binary
  binary="$(core_binary)"

  "$binary" "$MQTT_HOST" "$MQTT_PORT" "$BACKUP_CORE_ID" "$BACKUP_CORE_IP" "$BACKUP_CORE_PORT" \
    >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

start_edge() {
  local log_file="$1"
  local binary
  binary="$(edge_binary)"

  "$binary" "$EDGE_BIND_HOST" "$EDGE_NODE_PORT" "$EDGE_CORE_HOST" "$EDGE_CORE_PORT" \
    "$EDGE_BACKUP_CORE_IP" "$EDGE_BACKUP_CORE_PORT" >"$log_file" 2>&1 &
  register_pid "$!"
  printf '%s\n' "$!"
}

kill_forcefully() {
  local pid="$1"

  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

extract_core_id() {
  local log_file="$1"
  grep -E '^\[core\] [0-9a-f-]{36} \((ACTIVE|BACKUP)\) running' "$log_file" | head -n1 | awk '{print $2}'
}

extract_edge_id() {
  local log_file="$1"
  grep -E '^\[edge\] [0-9a-f-]{36}  local=' "$log_file" | head -n1 | awk '{print $2}'
}
