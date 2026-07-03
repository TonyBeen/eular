#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/stun_node"

usage() {
    cat <<'EOF'
Usage:
  node_start.sh --hub <hub_host:port> --node-id <node_id>
                [--public-host <ip_or_domain>]
                [--control-port <port>] [--stun-port <port>] [--stun-alt-port <port>]
                [--region <region>] [--mqtt-username <user>] [--mqtt-password <pass>]
                [--auth-secret <secret>] [--verbose]

Behavior:
  If --public-host is not provided, the script runs:
    curl -fsS myip.ipip.net
  and extracts the first IPv4 address as --public-host.

Environment fallback:
  NODE_HUB
  NODE_ID
  NODE_PUBLIC_HOST
  NODE_CONTROL_PORT
  NODE_STUN_PORT
  NODE_STUN_ALT_PORT
  NODE_REGION
  NODE_MQTT_USERNAME
  NODE_MQTT_PASSWORD
  NODE_AUTH_SECRET

Example:
  ./node_start.sh --hub bd.eular.top:1883 --node-id node-a --mqtt-username stun --mqtt-password 'secret'
EOF
}

require_bin() {
    if [[ ! -x "${BIN}" ]]; then
        echo "stun_node not found in script directory: ${BIN}" >&2
        exit 1
    fi
}

kill_old_process() {
    local node_id="$1"
    local pids

    if [[ -n "${node_id}" ]]; then
        pids="$(pgrep -f "${BIN}.*--node-id ${node_id}" || true)"
    else
        pids="$(pgrep -f "${BIN}" || true)"
    fi
    if [[ -n "${pids}" ]]; then
        echo "stopping existing stun_node: ${pids}"
        if [[ -n "${node_id}" ]]; then
            pkill -f "${BIN}.*--node-id ${node_id}" || true
        else
            pkill -f "${BIN}" || true
        fi
        sleep 1
    fi
}

detect_public_ip() {
    local output ip

    output="$(curl -fsS myip.ipip.net)"
    ip="$(printf '%s\n' "${output}" | grep -Eo '([0-9]{1,3}\.){3}[0-9]{1,3}' | head -n1 || true)"
    if [[ -z "${ip}" ]]; then
        echo "failed to detect public IPv4 from myip.ipip.net: ${output}" >&2
        return 1
    fi
    printf '%s\n' "${ip}"
}

HUB="${NODE_HUB:-}"
NODE_ID="${NODE_ID:-}"
PUBLIC_HOST="${NODE_PUBLIC_HOST:-}"
CONTROL_PORT="${NODE_CONTROL_PORT:-19000}"
STUN_PORT="${NODE_STUN_PORT:-3478}"
STUN_ALT_PORT="${NODE_STUN_ALT_PORT:-3479}"
REGION="${NODE_REGION:-default}"
MQTT_USERNAME="${NODE_MQTT_USERNAME:-}"
MQTT_PASSWORD="${NODE_MQTT_PASSWORD:-}"
AUTH_SECRET="${NODE_AUTH_SECRET:-}"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hub)
            HUB="${2:-}"
            shift 2
            ;;
        --node-id)
            NODE_ID="${2:-}"
            shift 2
            ;;
        --public-host)
            PUBLIC_HOST="${2:-}"
            shift 2
            ;;
        --control-port)
            CONTROL_PORT="${2:-}"
            shift 2
            ;;
        --stun-port)
            STUN_PORT="${2:-}"
            shift 2
            ;;
        --stun-alt-port)
            STUN_ALT_PORT="${2:-}"
            shift 2
            ;;
        --region)
            REGION="${2:-}"
            shift 2
            ;;
        --mqtt-username)
            MQTT_USERNAME="${2:-}"
            shift 2
            ;;
        --mqtt-password)
            MQTT_PASSWORD="${2:-}"
            shift 2
            ;;
        --auth-secret)
            AUTH_SECRET="${2:-}"
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

if [[ -z "${HUB}" || -z "${NODE_ID}" ]]; then
    echo "missing required arguments: --hub and --node-id" >&2
    usage
    exit 1
fi

require_bin
kill_old_process "${NODE_ID}"

if [[ -z "${PUBLIC_HOST}" ]]; then
    PUBLIC_HOST="$(detect_public_ip)"
    if [[ "${VERBOSE}" -eq 1 ]]; then
        echo "detected public host: ${PUBLIC_HOST}"
    fi
fi

CMD=(
    "${BIN}"
    --hub "${HUB}"
    --node-id "${NODE_ID}"
    --public-host "${PUBLIC_HOST}"
    --control-port "${CONTROL_PORT}"
    --stun-port "${STUN_PORT}"
    --stun-alt-port "${STUN_ALT_PORT}"
    --region "${REGION}"
)

if [[ -n "${MQTT_USERNAME}" ]]; then
    CMD+=(--mqtt-username "${MQTT_USERNAME}")
fi
if [[ -n "${MQTT_PASSWORD}" ]]; then
    CMD+=(--mqtt-password "${MQTT_PASSWORD}")
fi
if [[ -n "${AUTH_SECRET}" ]]; then
    CMD+=(--auth-secret "${AUTH_SECRET}")
fi
if [[ "${VERBOSE}" -eq 1 ]]; then
    CMD+=(--verbose)
    echo "exec: ${CMD[*]}"
fi

exec "${CMD[@]}"
