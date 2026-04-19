#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-}"
MOSQ_PID=""
MOSQ_LOG=""
BROKER_OWNED=0

cleanup() {
  if [[ "$BROKER_OWNED" == "1" && -n "$MOSQ_PID" ]] && kill -0 "$MOSQ_PID" 2>/dev/null; then
    kill "$MOSQ_PID" 2>/dev/null || true
    wait "$MOSQ_PID" 2>/dev/null || true
  fi
  if [[ -n "$MOSQ_LOG" && -f "$MOSQ_LOG" ]]; then
    rm -f "$MOSQ_LOG"
  fi
}

broker_is_up() {
  local host="${1:-$MQTT_HOST}"
  local port="${2:-$MQTT_PORT}"
  if command -v nc >/dev/null 2>&1; then
    nc -z "$host" "$port" >/dev/null 2>&1
    return $?
  fi
  if command -v lsof >/dev/null 2>&1; then
    lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1
    return $?
  fi
  return 1
}

pick_test_port() {
  if [[ -n "$MQTT_PORT" ]]; then
    printf '%s\n' "$MQTT_PORT"
    return 0
  fi

  local candidate
  for candidate in 18883 18884 18885 28883 28884; do
    if ! broker_is_up "$MQTT_HOST" "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  printf '18883\n'
}

start_owned_broker() {
  MOSQ_LOG="$(mktemp "${TMPDIR:-/tmp}/mqtt-publisher-failover.XXXXXX.log")"
  mosquitto -p "$MQTT_PORT" -v >"$MOSQ_LOG" 2>&1 &
  MOSQ_PID="$!"
  BROKER_OWNED=1

  local _i
  for _i in $(seq 1 50); do
    if broker_is_up "$MQTT_HOST" "$MQTT_PORT"; then
      return 0
    fi
    if ! kill -0 "$MOSQ_PID" 2>/dev/null; then
      printf '[test][fail] publisher/failover — owned mosquitto exited early\n' >&2
      cat "$MOSQ_LOG" >&2 || true
      return 1
    fi
    sleep 0.1
  done

  printf '[test][fail] publisher/failover — owned mosquitto did not open %s:%s\n' "$MQTT_HOST" "$MQTT_PORT" >&2
  cat "$MOSQ_LOG" >&2 || true
  return 1
}

trap cleanup EXIT

if ! command -v pytest >/dev/null 2>&1; then
  printf '[test][skip] publisher/failover — pytest not found\n'
  exit 0
fi

cd "$ROOT_DIR"
MQTT_PORT="$(pick_test_port)"
export MQTT_HOST MQTT_PORT

if ! broker_is_up "$MQTT_HOST" "$MQTT_PORT"; then
  if ! command -v mosquitto >/dev/null 2>&1; then
    printf '[test][skip] publisher/failover — MQTT broker not running on %s:%s and mosquitto not found\n' "$MQTT_HOST" "$MQTT_PORT"
    exit 0
  fi
  start_owned_broker
fi

PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 pytest -q test/integration/test_07_publisher_failover.py
printf '[test] publisher_failover completed\n'
