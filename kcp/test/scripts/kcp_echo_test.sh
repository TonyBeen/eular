#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${ROOT_DIR}/build"
SERVER_BIN=""
CLIENT_BIN=""
SERVER_IP="127.0.0.1"
PORT_BASE=25421
ROUNDS=1
COUNT=8
TIMEOUT_SEC=20
KEEP_LOGS=0
LENGTHS=("1" "64" "512" "1200" "2048" "4096")
NIC=""

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  -b <build_dir>     Build directory. Default: ${BUILD_DIR}
  -p <port_base>     First server port. Default: ${PORT_BASE}
  -r <rounds>        Repeat each length N times. Default: ${ROUNDS}
  -c <count>         Echo messages per case. Default: ${COUNT}
  -t <timeout_sec>   Per-case timeout in seconds. Default: ${TIMEOUT_SEC}
  -l <csv_lengths>   Message lengths, comma separated. Default: ${LENGTHS[*]}
  -n <nic>           Optional NIC passed to server/client
  -k                 Keep log directory
  -h                 Show help
EOF
}

while getopts ":b:p:r:c:t:l:n:kh" opt; do
    case "${opt}" in
        b) BUILD_DIR="${OPTARG}" ;;
        p) PORT_BASE="${OPTARG}" ;;
        r) ROUNDS="${OPTARG}" ;;
        c) COUNT="${OPTARG}" ;;
        t) TIMEOUT_SEC="${OPTARG}" ;;
        l) IFS=',' read -r -a LENGTHS <<< "${OPTARG}" ;;
        n) NIC="${OPTARG}" ;;
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

if [[ ! -x "${SERVER_BIN}" || ! -x "${CLIENT_BIN}" ]]; then
    echo "building required targets in ${BUILD_DIR}"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" -j4 --target kcp_server.out kcp_echo_test_client.out
fi

LOG_DIR=$(mktemp -d "/tmp/kcp_echo_test_XXXXXX")
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    if [[ "${KEEP_LOGS}" -eq 0 ]]; then
        rm -rf "${LOG_DIR}"
    else
        echo "logs kept in ${LOG_DIR}"
    fi
}
trap cleanup EXIT

run_case() {
    local length="$1"
    local round="$2"
    local port="$3"
    local case_id="len_${length}_round_${round}"
    local server_log="${LOG_DIR}/${case_id}.server.log"
    local client_log="${LOG_DIR}/${case_id}.client.log"

    echo "=== case ${case_id} port=${port} count=${COUNT} ==="

    local server_cmd=("${SERVER_BIN}" "-p" "${port}")
    local client_cmd=("${CLIENT_BIN}" "-s" "${SERVER_IP}" "-p" "${port}" "-c" "${COUNT}" "-m" "${length}" "-t" "$((TIMEOUT_SEC * 1000))")
    if [[ -n "${NIC}" ]]; then
        server_cmd+=("-n" "${NIC}")
        client_cmd+=("-n" "${NIC}")
    fi

    "${server_cmd[@]}" >"${server_log}" 2>&1 &
    SERVER_PID=$!
    sleep 1

    if ! timeout "${TIMEOUT_SEC}" "${client_cmd[@]}" >"${client_log}" 2>&1; then
        echo "client failed for ${case_id}" >&2
        echo "--- client log ---" >&2
        sed -n '1,200p' "${client_log}" >&2
        echo "--- server log ---" >&2
        sed -n '1,200p' "${server_log}" >&2
        return 1
    fi

    if ! grep -q "RESULT ok" "${client_log}"; then
        echo "missing success marker in ${client_log}" >&2
        sed -n '1,200p' "${client_log}" >&2
        return 1
    fi

    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""

    echo "PASS ${case_id}"
}

port="${PORT_BASE}"
for length in "${LENGTHS[@]}"; do
    for ((round = 1; round <= ROUNDS; round++)); do
        run_case "${length}" "${round}" "${port}"
        port=$((port + 1))
    done
done

echo "ALL PASS lengths=${LENGTHS[*]} rounds=${ROUNDS} count=${COUNT}"
