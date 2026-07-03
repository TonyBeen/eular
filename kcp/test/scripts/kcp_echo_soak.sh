#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${ROOT_DIR}/build"
SERVER_BIN=""
CLIENT_BIN=""
SERVER_IP="127.0.0.1"
PORT_BASE=26421
ROUNDS=1
DURATION_SEC=60
TIMEOUT_SLACK_SEC=20
CONCURRENCY=1
LENGTHS=("64" "512" "1200" "4096")
NIC=""
KEEP_LOGS=0

NETEM_IFACE="lo"
NETEM_DELAY=""
NETEM_JITTER=""
NETEM_LOSS=""
NETEM_REORDER=""
NETEM_DUPLICATE=""
NETEM_CORRUPT=""
PRIV_CMD=()

LOG_DIR=""
SERVER_PID=""
NETEM_ACTIVE=0

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  -b <build_dir>      Build directory. Default: ${BUILD_DIR}
  -s <server_ip>      Server IP. Default: ${SERVER_IP}
  -p <port_base>      First server port. Default: ${PORT_BASE}
  -r <rounds>         Repeat each length N times. Default: ${ROUNDS}
  -d <duration_sec>   Soak duration per case. Default: ${DURATION_SEC}
  -t <slack_sec>      Extra timeout over duration. Default: ${TIMEOUT_SLACK_SEC}
  -P <clients>        Concurrent clients per case. Default: ${CONCURRENCY}
  -l <csv_lengths>    Message lengths. Default: ${LENGTHS[*]}
  -n <nic>            Optional NIC passed to server/client
  -i <iface>          Interface for tc netem. Default: ${NETEM_IFACE}
  -D <delay>          Netem delay, e.g. 40ms
  -J <jitter>         Netem jitter, e.g. 5ms
  -L <loss>           Netem loss, e.g. 1%
  -R <reorder>        Netem reorder, e.g. '25% 50%'
  -U <duplicate>      Netem duplicate, e.g. 0.2%
  -C <corrupt>        Netem corrupt, e.g. 0.1%
  -k                  Keep logs
  -h                  Show help
EOF
}

while getopts ":b:s:p:r:d:t:P:l:n:i:D:J:L:R:U:C:kh" opt; do
    case "${opt}" in
        b) BUILD_DIR="${OPTARG}" ;;
        s) SERVER_IP="${OPTARG}" ;;
        p) PORT_BASE="${OPTARG}" ;;
        r) ROUNDS="${OPTARG}" ;;
        d) DURATION_SEC="${OPTARG}" ;;
        t) TIMEOUT_SLACK_SEC="${OPTARG}" ;;
        P) CONCURRENCY="${OPTARG}" ;;
        l) IFS=',' read -r -a LENGTHS <<< "${OPTARG}" ;;
        n) NIC="${OPTARG}" ;;
        i) NETEM_IFACE="${OPTARG}" ;;
        D) NETEM_DELAY="${OPTARG}" ;;
        J) NETEM_JITTER="${OPTARG}" ;;
        L) NETEM_LOSS="${OPTARG}" ;;
        R) NETEM_REORDER="${OPTARG}" ;;
        U) NETEM_DUPLICATE="${OPTARG}" ;;
        C) NETEM_CORRUPT="${OPTARG}" ;;
        k) KEEP_LOGS=1 ;;
        h)
            usage
            exit 0
            ;;
        :)
            echo "missing argument for -${OPTARG}" >&2
            exit 2
            ;;
        \?)
            echo "unknown option: -${OPTARG}" >&2
            exit 2
            ;;
    esac
done

SERVER_BIN="${BUILD_DIR}/examples/kcp_server.out"
CLIENT_BIN="${BUILD_DIR}/test/kcp_echo_test_client.out"

require_build() {
    if [[ ! -x "${SERVER_BIN}" || ! -x "${CLIENT_BIN}" ]]; then
        echo "building required targets in ${BUILD_DIR}"
        cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
        cmake --build "${BUILD_DIR}" -j4 --target kcp_server.out kcp_echo_test_client.out
    fi
}

cleanup_server() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
}

clear_netem() {
    if [[ "${NETEM_ACTIVE}" -eq 1 ]]; then
        "${PRIV_CMD[@]}" tc qdisc del dev "${NETEM_IFACE}" root >/dev/null 2>&1 || true
        NETEM_ACTIVE=0
    fi
}

cleanup() {
    cleanup_server
    clear_netem
    if [[ -n "${LOG_DIR}" ]]; then
        if [[ "${KEEP_LOGS}" -eq 0 ]]; then
            rm -rf "${LOG_DIR}"
        else
            echo "logs kept in ${LOG_DIR}"
        fi
    fi
}
trap cleanup EXIT

apply_netem() {
    local args=()
    if [[ -n "${NETEM_DELAY}" ]]; then
        args+=(delay "${NETEM_DELAY}")
        if [[ -n "${NETEM_JITTER}" ]]; then
            args+=("${NETEM_JITTER}")
        fi
    fi
    if [[ -n "${NETEM_LOSS}" ]]; then
        args+=(loss "${NETEM_LOSS}")
    fi
    if [[ -n "${NETEM_REORDER}" ]]; then
        read -r -a reorder_parts <<< "${NETEM_REORDER}"
        args+=(reorder "${reorder_parts[@]}")
    fi
    if [[ -n "${NETEM_DUPLICATE}" ]]; then
        args+=(duplicate "${NETEM_DUPLICATE}")
    fi
    if [[ -n "${NETEM_CORRUPT}" ]]; then
        args+=(corrupt "${NETEM_CORRUPT}")
    fi

    if [[ "${#args[@]}" -eq 0 ]]; then
        return
    fi

    if [[ "${#PRIV_CMD[@]}" -eq 0 ]]; then
        if [[ "${EUID}" -eq 0 ]]; then
            PRIV_CMD=()
        elif sudo -n true >/dev/null 2>&1; then
            PRIV_CMD=(sudo -n)
        else
            echo "netem requires root or passwordless sudo; rerun with sudo or preconfigure sudo -n" >&2
            exit 1
        fi
    fi

    echo "applying netem on ${NETEM_IFACE}: ${args[*]}"
    "${PRIV_CMD[@]}" tc qdisc replace dev "${NETEM_IFACE}" root netem "${args[@]}"
    NETEM_ACTIVE=1
}

run_case() {
    local length="$1"
    local round="$2"
    local port="$3"
    local case_id="soak_len_${length}_round_${round}"
    local server_log="${LOG_DIR}/${case_id}.server.log"
    local timeout_sec=$((DURATION_SEC + TIMEOUT_SLACK_SEC))
    local client_pids=()
    local client_logs=()
    local client_status=0

    echo "=== case ${case_id} port=${port} duration=${DURATION_SEC}s len=${length} clients=${CONCURRENCY} ==="

    local server_cmd=("${SERVER_BIN}" "-p" "${port}")
    local client_cmd=(
        "${CLIENT_BIN}"
        "-s" "${SERVER_IP}"
        "-p" "${port}"
        "-c" "0"
        "-d" "$((DURATION_SEC * 1000))"
        "-m" "${length}"
        "-t" "$((timeout_sec * 1000))"
    )
    if [[ -n "${NIC}" ]]; then
        server_cmd+=("-n" "${NIC}")
        client_cmd+=("-n" "${NIC}")
    fi

    "${server_cmd[@]}" >"${server_log}" 2>&1 &
    SERVER_PID=$!
    sleep 1

    for ((client_idx = 0; client_idx < CONCURRENCY; client_idx++)); do
        local client_log="${LOG_DIR}/${case_id}.client_${client_idx}.log"
        local local_port=$((port + 1000 + client_idx))
        client_logs+=("${client_log}")
        timeout "${timeout_sec}" "${client_cmd[@]}" -l "${local_port}" >"${client_log}" 2>&1 &
        client_pids+=($!)
    done

    for ((client_idx = 0; client_idx < CONCURRENCY; client_idx++)); do
        if ! wait "${client_pids[client_idx]}"; then
            client_status=1
            echo "client ${client_idx} failed for ${case_id}" >&2
            echo "--- client ${client_idx} log ---" >&2
            sed -n '1,200p' "${client_logs[client_idx]}" >&2
        elif ! grep -q "RESULT ok" "${client_logs[client_idx]}"; then
            client_status=1
            echo "missing success marker in ${client_logs[client_idx]}" >&2
            sed -n '1,200p' "${client_logs[client_idx]}" >&2
        fi
    done

    if [[ "${client_status}" -ne 0 ]]; then
        echo "--- server log ---" >&2
        sed -n '1,200p' "${server_log}" >&2
        return 1
    fi

    cleanup_server
    echo "PASS ${case_id}"
    for client_log in "${client_logs[@]}"; do
        grep "RESULT ok" "${client_log}"
    done
}

require_build
LOG_DIR=$(mktemp -d "/tmp/kcp_echo_soak_XXXXXX")
apply_netem

if [[ "${CONCURRENCY}" -le 0 ]]; then
    echo "concurrency must be > 0" >&2
    exit 2
fi

port="${PORT_BASE}"
for length in "${LENGTHS[@]}"; do
    for ((round = 1; round <= ROUNDS; round++)); do
        run_case "${length}" "${round}" "${port}"
        port=$((port + 1))
    done
done

echo "ALL PASS soak lengths=${LENGTHS[*]} rounds=${ROUNDS} duration=${DURATION_SEC}s clients=${CONCURRENCY}"
