#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/ntrs_hub"

usage() {
    cat <<'EOF'
Usage:
  hub_start.sh [--host <listen_host>] [--port <listen_port>]
               [--state-file <path>] [--auth-secret <secret>] [--verbose]

Environment fallback:
  HUB_HOST
  HUB_PORT
  HUB_STATE_FILE
  HUB_AUTH_SECRET

Defaults:
  --host ::
  --port 18083
  --state-file ./ntrs_hub_state.bin

Example:
  ./hub_start.sh --host :: --port 18083 --auth-secret 'secret'
EOF
}

require_bin() {
    if [[ ! -x "${BIN}" ]]; then
        echo "ntrs_hub not found in script directory: ${BIN}" >&2
        exit 1
    fi
}

kill_old_process() {
    local pids

    pids="$(pgrep -f "${BIN}" || true)"
    if [[ -n "${pids}" ]]; then
        echo "stopping existing ntrs_hub: ${pids}"
        pkill -f "${BIN}" || true
        sleep 1
    fi
}

HOST="${HUB_HOST:-::}"
PORT="${HUB_PORT:-18083}"
STATE_FILE="${HUB_STATE_FILE:-./ntrs_hub_state.bin}"
AUTH_SECRET="${HUB_AUTH_SECRET:-}"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            HOST="${2:-}"
            shift 2
            ;;
        --host=*)
            HOST="${1#*=}"
            shift
            ;;
        --port)
            PORT="${2:-}"
            shift 2
            ;;
        --port=*)
            PORT="${1#*=}"
            shift
            ;;
        --state-file)
            STATE_FILE="${2:-}"
            shift 2
            ;;
        --state-file=*)
            STATE_FILE="${1#*=}"
            shift
            ;;
        --auth-secret)
            AUTH_SECRET="${2:-}"
            shift 2
            ;;
        --auth-secret=*)
            AUTH_SECRET="${1#*=}"
            shift
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

if [[ -z "${HOST}" || -z "${PORT}" || -z "${STATE_FILE}" ]]; then
    echo "missing required hub argument" >&2
    usage
    exit 1
fi

require_bin
kill_old_process

CMD=("${BIN}" --host "${HOST}" --port "${PORT}" --state-file "${STATE_FILE}")
if [[ -n "${AUTH_SECRET}" ]]; then
    CMD+=(--auth-secret "${AUTH_SECRET}")
fi

if [[ "${VERBOSE}" -eq 1 ]]; then
    echo "exec: ${CMD[*]}"
fi

exec "${CMD[@]}"
