#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/ntrs_node"

usage() {
    cat <<'EOT'
Usage:
  node_start.sh [ntrs_node options]

Behavior:
  The script forwards all arguments to ntrs_node unchanged.

  For operational convenience, it only auto-fills arguments that are not
  already provided on the command line:
    --node-id          from NODE_ID, or generated as node-<hostname>-<time>
    --public-host      from NODE_PUBLIC_HOST, or detected by curl
    --control-port     from NODE_CONTROL_PORT when set
    --probe-port       from NODE_PROBE_PORT/NODE_STUN_PORT when set
    --probe-alt-port   from NODE_PROBE_ALT_PORT/NODE_STUN_ALT_PORT when set
    --bind-ip          from NODE_BIND_IP when set
    --bind-device      from NODE_BIND_DEVICE when set
    --region           from NODE_REGION when set
    --auth-secret      from NODE_AUTH_SECRET when set
    --hub              from NODE_HUB when set
    --mqtt-username    from NODE_MQTT_USERNAME when set
    --mqtt-password    from NODE_MQTT_PASSWORD when set
    -4/-6              from NODE_ADDRESS_FAMILY when set

  In IPv4 mode it runs "curl -fsS myip.ipip.net -4" for --public-host.
  In IPv6 mode it runs "curl -fsS myip.ipip.net -6" for --public-host.
  If --bind-device/--bind-ip is provided, public-host detection also passes
  that value to curl through --interface.

Environment fallback:
  NODE_HUB
  NODE_ID
  NODE_PUBLIC_HOST
  NODE_CONTROL_PORT
  NODE_PROBE_PORT
  NODE_PROBE_ALT_PORT
  NODE_STUN_PORT
  NODE_STUN_ALT_PORT
  NODE_BIND_IP
  NODE_BIND_DEVICE
  NODE_REGION
  NODE_MQTT_USERNAME
  NODE_MQTT_PASSWORD
  NODE_AUTH_SECRET
  NODE_ADDRESS_FAMILY

Compatibility:
  NODE_STUN_PORT/NODE_STUN_ALT_PORT are treated as environment aliases for
  NODE_PROBE_PORT/NODE_PROBE_ALT_PORT.

Example:
  ./node_start.sh --hub bd.eular.top:1883 --mqtt-username ntrs --mqtt-password 'secret' -6 --verbose
EOT
}

require_bin() {
    if [[ ! -x "${BIN}" ]]; then
        echo "ntrs_node not found in script directory: ${BIN}" >&2
        exit 1
    fi
}

kill_old_process() {
    local node_id="$1"
    local pids

    if [[ -n "${node_id}" ]]; then
        pids="$(pgrep -f "${BIN}.*--node-id ${node_id}" 2>/dev/null || true)"
    else
        pids="$(pgrep -f "${BIN}" 2>/dev/null || true)"
    fi
    if [[ -n "${pids}" ]]; then
        echo "stopping existing ntrs_node: ${pids}"
        if [[ -n "${node_id}" ]]; then
            pkill -f "${BIN}.*--node-id ${node_id}" 2>/dev/null || true
        else
            pkill -f "${BIN}" 2>/dev/null || true
        fi
        sleep 1
    fi
}

detect_public_ip() {
    local output ip
    local family="${1:-4}"
    local bind_interface="${2:-}"
    local curl_args=(-fsS myip.ipip.net)

    if [[ -n "${bind_interface}" ]]; then
        curl_args+=(--interface "${bind_interface}")
    fi

    if [[ "${family}" == "6" ]]; then
        output="$(curl "${curl_args[@]}" -6)"
        ip="$(printf '%s\n' "${output}" | grep -Eio '([0-9a-f]{1,4}:){2,}[0-9a-f:]{0,}' | head -n1 || true)"
    else
        output="$(curl "${curl_args[@]}" -4)"
        ip="$(printf '%s\n' "${output}" | grep -Eo '([0-9]{1,3}\.){3}[0-9]{1,3}' | head -n1 || true)"
    fi
    if [[ -z "${ip}" ]]; then
        echo "failed to detect public IPv${family} from myip.ipip.net: ${output}" >&2
        return 1
    fi
    printf '%s\n' "${ip}"
}

option_value() {
    local arg="$1"
    local name="$2"

    if [[ "${arg}" == "${name}="* ]]; then
        printf '%s\n' "${arg#*=}"
        return 0
    fi
    return 1
}

has_option=0
has_hub=0
has_node_id=0
has_public_host=0
has_control_port=0
has_probe_port=0
has_probe_alt_port=0
has_bind_ip=0
has_bind_device=0
has_region=0
has_mqtt_username=0
has_mqtt_password=0
has_auth_secret=0
has_address_family=0
verbose=0
node_id_arg=""
address_family="${NODE_ADDRESS_FAMILY:-4}"
bind_ip_arg="${NODE_BIND_IP:-}"
bind_device_arg="${NODE_BIND_DEVICE:-}"

args=("$@")
i=0
while [[ ${i} -lt ${#args[@]} ]]; do
    arg="${args[${i}]}"
    next=""
    if [[ $((i + 1)) -lt ${#args[@]} ]]; then
        next="${args[$((i + 1))]}"
    fi
    if [[ "${arg}" == -* ]]; then
        has_option=1
    fi

    case "${arg}" in
        --help|-h)
            usage
            exit 0
            ;;
        --hub)
            has_hub=1
            ((i += 2))
            continue
            ;;
        --hub=*)
            has_hub=1
            ;;
        --node-id)
            has_node_id=1
            node_id_arg="${next}"
            ((i += 2))
            continue
            ;;
        --node-id=*)
            has_node_id=1
            node_id_arg="$(option_value "${arg}" "--node-id")"
            ;;
        --public-host|--advertise-host)
            has_public_host=1
            ((i += 2))
            continue
            ;;
        --public-host=*|--advertise-host=*)
            has_public_host=1
            ;;
        --control-port)
            has_control_port=1
            ((i += 2))
            continue
            ;;
        --control-port=*)
            has_control_port=1
            ;;
        --probe-port|--stun-port)
            has_probe_port=1
            ((i += 2))
            continue
            ;;
        --probe-port=*|--stun-port=*)
            has_probe_port=1
            ;;
        --probe-alt-port|--stun-alt-port)
            has_probe_alt_port=1
            ((i += 2))
            continue
            ;;
        --probe-alt-port=*|--stun-alt-port=*)
            has_probe_alt_port=1
            ;;
        --bind-ip)
            has_bind_ip=1
            bind_ip_arg="${next}"
            ((i += 2))
            continue
            ;;
        --bind-ip=*)
            has_bind_ip=1
            bind_ip_arg="$(option_value "${arg}" "--bind-ip")"
            ;;
        --bind-device)
            has_bind_device=1
            bind_device_arg="${next}"
            ((i += 2))
            continue
            ;;
        --bind-device=*)
            has_bind_device=1
            bind_device_arg="$(option_value "${arg}" "--bind-device")"
            ;;
        --region)
            has_region=1
            ((i += 2))
            continue
            ;;
        --region=*)
            has_region=1
            ;;
        --mqtt-username|--Mqtt-username)
            has_mqtt_username=1
            ((i += 2))
            continue
            ;;
        --mqtt-username=*|--Mqtt-username=*)
            has_mqtt_username=1
            ;;
        --mqtt-password|--Mqtt-password)
            has_mqtt_password=1
            ((i += 2))
            continue
            ;;
        --mqtt-password=*|--Mqtt-password=*)
            has_mqtt_password=1
            ;;
        --auth-secret)
            has_auth_secret=1
            ((i += 2))
            continue
            ;;
        --auth-secret=*)
            has_auth_secret=1
            ;;
        --ipv4|-4)
            has_address_family=1
            address_family=4
            ;;
        --ipv6|-6)
            has_address_family=1
            address_family=6
            ;;
        --verbose|-v)
            verbose=1
            ;;
    esac
    ((++i))
done

if [[ ${has_option} -eq 0 && $# -gt 0 ]]; then
    echo "legacy positional arguments are forwarded unchanged; environment auto-fill is disabled" >&2
    require_bin
    exec "${BIN}" "$@"
fi

cmd=("${BIN}" "$@")

if [[ ${has_hub} -eq 0 && -n "${NODE_HUB:-}" ]]; then
    cmd+=(--hub "${NODE_HUB}")
    has_hub=1
fi
if [[ ${has_hub} -eq 0 ]]; then
    echo "missing required argument: --hub" >&2
    usage
    exit 1
fi

if [[ ${has_node_id} -eq 0 ]]; then
    node_id_arg="${NODE_ID:-}"
    if [[ -z "${node_id_arg}" ]]; then
        node_id_arg="node-$(hostname)-$(date +%s)"
        if [[ ${verbose} -eq 1 ]]; then
            echo "auto generated node-id: ${node_id_arg}"
        fi
    fi
    cmd+=(--node-id "${node_id_arg}")
fi

if [[ ${has_public_host} -eq 0 ]]; then
    public_host="${NODE_PUBLIC_HOST:-}"
    if [[ -z "${public_host}" ]]; then
        public_host="$(detect_public_ip "${address_family}" "${bind_device_arg:-${bind_ip_arg:-}}")"
        if [[ ${verbose} -eq 1 ]]; then
            echo "detected public host: ${public_host}"
        fi
    fi
    cmd+=(--public-host "${public_host}")
fi

if [[ ${has_control_port} -eq 0 && -n "${NODE_CONTROL_PORT:-}" ]]; then
    cmd+=(--control-port "${NODE_CONTROL_PORT}")
fi
if [[ ${has_probe_port} -eq 0 && -n "${NODE_PROBE_PORT:-${NODE_STUN_PORT:-}}" ]]; then
    cmd+=(--probe-port "${NODE_PROBE_PORT:-${NODE_STUN_PORT:-}}")
fi
if [[ ${has_probe_alt_port} -eq 0 && -n "${NODE_PROBE_ALT_PORT:-${NODE_STUN_ALT_PORT:-}}" ]]; then
    cmd+=(--probe-alt-port "${NODE_PROBE_ALT_PORT:-${NODE_STUN_ALT_PORT:-}}")
fi
if [[ ${has_bind_ip} -eq 0 && -n "${NODE_BIND_IP:-}" ]]; then
    cmd+=(--bind-ip "${NODE_BIND_IP}")
fi
if [[ ${has_bind_device} -eq 0 && -n "${NODE_BIND_DEVICE:-}" ]]; then
    cmd+=(--bind-device "${NODE_BIND_DEVICE}")
fi
if [[ ${has_region} -eq 0 && -n "${NODE_REGION:-}" ]]; then
    cmd+=(--region "${NODE_REGION}")
fi
if [[ ${has_address_family} -eq 0 && -n "${NODE_ADDRESS_FAMILY:-}" ]]; then
    if [[ "${address_family}" == "6" ]]; then
        cmd+=(--ipv6)
    else
        cmd+=(--ipv4)
    fi
fi
if [[ ${has_mqtt_username} -eq 0 && -n "${NODE_MQTT_USERNAME:-}" ]]; then
    cmd+=(--mqtt-username "${NODE_MQTT_USERNAME}")
fi
if [[ ${has_mqtt_password} -eq 0 && -n "${NODE_MQTT_PASSWORD:-}" ]]; then
    cmd+=(--mqtt-password "${NODE_MQTT_PASSWORD}")
fi
if [[ ${has_auth_secret} -eq 0 && -n "${NODE_AUTH_SECRET:-}" ]]; then
    cmd+=(--auth-secret "${NODE_AUTH_SECRET}")
fi

require_bin
if [[ -n "${node_id_arg}" ]]; then
    kill_old_process "${node_id_arg}"
fi

if [[ ${verbose} -eq 1 ]]; then
    echo "exec: ${cmd[*]}"
fi

exec "${cmd[@]}"
