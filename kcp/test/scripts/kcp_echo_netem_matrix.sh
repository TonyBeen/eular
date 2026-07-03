#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SOAK_SCRIPT="${ROOT_DIR}/test/scripts/kcp_echo_soak.sh"
KEEP_LOGS=0
BUILD_DIR=""
NIC=""
IFACE="lo"
DURATION_SEC=30
CONCURRENCY=2
LENGTHS="64,512,1200"
PORT_BASE=27421

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  -b <build_dir>    Build directory
  -d <duration>     Duration per scenario in seconds. Default: ${DURATION_SEC}
  -P <clients>      Concurrent clients per scenario. Default: ${CONCURRENCY}
  -l <lengths>      CSV message lengths. Default: ${LENGTHS}
  -p <port_base>    Base port. Default: ${PORT_BASE}
  -n <nic>          Optional NIC passed to server/client
  -i <iface>        Netem interface. Default: ${IFACE}
  -k                Keep logs
  -h                Show help
EOF
}

while getopts ":b:d:P:l:p:n:i:kh" opt; do
    case "${opt}" in
        b) BUILD_DIR="${OPTARG}" ;;
        d) DURATION_SEC="${OPTARG}" ;;
        P) CONCURRENCY="${OPTARG}" ;;
        l) LENGTHS="${OPTARG}" ;;
        p) PORT_BASE="${OPTARG}" ;;
        n) NIC="${OPTARG}" ;;
        i) IFACE="${OPTARG}" ;;
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

if [[ ! -x "${SOAK_SCRIPT}" ]]; then
    echo "missing soak script: ${SOAK_SCRIPT}" >&2
    exit 1
fi

run_scenario() {
    local name="$1"
    shift
    local cmd=("${SOAK_SCRIPT}" -d "${DURATION_SEC}" -P "${CONCURRENCY}" -l "${LENGTHS}" -p "${PORT_BASE}")
    if [[ -n "${BUILD_DIR}" ]]; then
        cmd+=(-b "${BUILD_DIR}")
    fi
    if [[ -n "${NIC}" ]]; then
        cmd+=(-n "${NIC}")
    fi
    if [[ "${KEEP_LOGS}" -eq 1 ]]; then
        cmd+=(-k)
    fi
    cmd+=(-i "${IFACE}")
    cmd+=("$@")

    echo
    echo "##### scenario: ${name} #####"
    "${cmd[@]}"
    PORT_BASE=$((PORT_BASE + 100))
}

run_scenario "baseline" 
run_scenario "delay_jitter" -D 25ms -J 5ms
run_scenario "loss" -L 1%
run_scenario "delay_loss" -D 40ms -J 10ms -L 2%
run_scenario "reorder" -D 20ms -J 5ms -R "20% 50%"

echo
echo "NETEM MATRIX PASS duration=${DURATION_SEC}s clients=${CONCURRENCY} lengths=${LENGTHS}"
