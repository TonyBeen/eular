#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <build-dir> [clients] [duration-seconds] [count] [length] [port]" >&2
    exit 1
fi

BUILD_DIR=$1
CLIENTS=${2:-128}
DURATION_SECONDS=${3:-35}
COUNT=${4:-10}
LENGTH=${5:-64}
PORT=${6:-20023}
SUDO_PASSWORD=${SUDO_PASSWORD:-1}

SERVER_BIN="${BUILD_DIR}/examples/utp_echo_server"
CLIENT_BIN="${BUILD_DIR}/examples/utp_echo_client"
FLAMEGRAPH_DIR="/home/eular/VSCode/FlameGraph"
SERVER_LOG="/tmp/utp_echo_server_silent.log"
PERF_LOG="/tmp/utp_echo_perf.record.log"
PERF_DATA="/tmp/utp_echo_perf.data"
PERF_FOLDED="/tmp/utp_echo_perf.folded"
FLAME_SVG="/tmp/utp_echo_flame.svg"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "server binary not found: $SERVER_BIN" >&2
    exit 1
fi

if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "client binary not found: $CLIENT_BIN" >&2
    exit 1
fi

SERVER_PID=""
PERF_BG_PID=""

cleanup() {
    pkill -f "$CLIENT_BIN --server-ip 127.0.0.1 --server-port $PORT --count $COUNT --length $LENGTH --silent" || true
    pkill -f "$SERVER_BIN --bind-ip 127.0.0.1 --bind-port $PORT --silent" || true
    if [[ -n "$SERVER_PID" ]]; then
        pkill -f "perf record -F 99 -g -o $PERF_DATA -p $SERVER_PID -- sleep $DURATION_SECONDS" || true
    fi
}

trap cleanup EXIT

rm -f "$SERVER_LOG" "$PERF_LOG" "$PERF_FOLDED" "$FLAME_SVG"
printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' rm -f "$PERF_DATA" >/dev/null 2>&1 || true

"$SERVER_BIN" --bind-ip 127.0.0.1 --bind-port "$PORT" --silent >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

sleep 2

( printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' perf record -F 99 -g -o "$PERF_DATA" -p "$SERVER_PID" -- sleep "$DURATION_SECONDS" >"$PERF_LOG" 2>&1 ) &
PERF_BG_PID=$!

sleep 2

CLIENT_PIDS=()
for _ in $(seq 1 "$CLIENTS"); do
    "$CLIENT_BIN" --server-ip 127.0.0.1 --server-port "$PORT" --count "$COUNT" --length "$LENGTH" --silent >/dev/null 2>&1 &
    CLIENT_PIDS+=($!)
done

for pid in "${CLIENT_PIDS[@]}"; do
    wait "$pid"
done

wait "$PERF_BG_PID"

printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' chown "$(id -u):$(id -g)" "$PERF_DATA" >/dev/null 2>&1
perf script -i "$PERF_DATA" | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" > "$PERF_FOLDED"
"$FLAMEGRAPH_DIR/flamegraph.pl" "$PERF_FOLDED" > "$FLAME_SVG"

echo "flamegraph=$FLAME_SVG"
echo "perf_data=$PERF_DATA"
echo "server_log=$SERVER_LOG"
