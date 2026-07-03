#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/stun_hub"

usage() {
    cat <<'EOF'
Usage:
  hub_start.sh --host <mqtt_host> [--port <mqtt_port>] [--username <user>] [--password <pass>] [--verbose]

Environment fallback:
  HUB_HOST
  HUB_PORT
  HUB_USERNAME
  HUB_PASSWORD

Example:
  ./hub_start.sh --host bd.eular.top --port 1883 --username stun --password 'secret'
EOF
}

require_bin() {
    if [[ ! -x "${BIN}" ]]; then
        echo "stun_hub not found in script directory: ${BIN}" >&2
        exit 1
    fi
}

kill_old_process() {
    local pids

    pids="$(pgrep -f "${BIN}" || true)"
    if [[ -n "${pids}" ]]; then
        echo "stopping existing stun_hub: ${pids}"
        pkill -f "${BIN}" || true
        sleep 1
    fi
}

HOST="${HUB_HOST:-}"
PORT="${HUB_PORT:-1883}"
USERNAME="${HUB_USERNAME:-}"
PASSWORD="${HUB_PASSWORD:-}"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            HOST="${2:-}"
            shift 2
            ;;
        --port)
            PORT="${2:-}"
            shift 2
            ;;
        --username)
            USERNAME="${2:-}"
            shift 2
            ;;
        --password)
            PASSWORD="${2:-}"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${HOST}" ]]; then
    echo "missing MQTT host" >&2
    usage
    exit 1
fi

require_bin
kill_old_process

CMD=("${BIN}" --host "${HOST}" --port "${PORT}")
if [[ -n "${USERNAME}" ]]; then
    CMD+=(--username "${USERNAME}")
fi
if [[ -n "${PASSWORD}" ]]; then
    CMD+=(--password "${PASSWORD}")
fi

if [[ "${VERBOSE}" -eq 1 ]]; then
    echo "exec: ${CMD[*]}"
fi

exec "${CMD[@]}"
