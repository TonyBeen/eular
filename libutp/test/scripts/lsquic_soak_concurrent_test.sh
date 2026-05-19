#!/usr/bin/env bash
# lsquic_soak_concurrent_test.sh — lsquic 并发长稳定性测试（纯基线，无 netem）
# 用法: ./test/scripts/lsquic_soak_concurrent_test.sh <build-dir> [options]

set -euo pipefail

BUILD_DIR=""
DURATION_SECONDS=600
NUM_CLIENTS=10
BYTES_PER_CLIENT=4194304
PAYLOAD_LENGTH=1024
REPORT_INTERVAL=10
SERVER_PORT=""
SERVER_PORT_AUTO=1
KEEP_LOGS=0
WORKER_PIDS=()
ECHO_CLIENT_BIN=""
RSS_WARN_THRESHOLD_KB=65536
RSS_FAIL_THRESHOLD_KB=0
CPU_STOP_THRESHOLD=80
CLIENT_TIMEOUT_SECONDS=90

SCRIPT_NAME="$(basename "$0")"
START_TS="$(date +%Y%m%d_%H%M%S)"
TMP_ROOT="/tmp/lsquic_soak_${START_TS}_$$"
REPORT_FILE="$TMP_ROOT/report.log"
SERVER_LOG="$TMP_ROOT/server.log"
CLIENT_LOG_DIR="$TMP_ROOT/clients"
CLIENT_STATE_DIR="$TMP_ROOT/state"
SERVER_PID=0
SERVER_START_RSS_KB=0
SERVER_START_FD=0
SERVER_PEAK_RSS_KB=0
SERVER_PEAK_FD=0
SERVER_EXITED_EARLY=0

usage() {
    cat <<EOF
用法:
  ./$SCRIPT_NAME <build-dir> [options]

选项:
  --duration-seconds <n>   运行时长，默认 600
  --clients <n>            并发 client 数，默认 10
  --bytes-per-client <n>   每轮每个 client 发送字节数，默认 4194304 (4 MiB)
  --length <n>             每轮发送块大小，默认 1024
  --report-interval <n>    采样周期（秒），默认 10
  --server-port <n>        server 监听端口；默认每次运行自动选择不同端口
  --rss-warn-threshold-kb <n>
                           RSS 告警阈值（相对起始值），默认 65536
  --rss-fail-threshold-kb <n>
                           RSS 失败阈值（相对起始值），默认 0（关闭）
  --cpu-stop-threshold <n> server CPU%% 超过该值时立刻停止，默认 80（关闭请设 0）
  --client-timeout <n>     单轮 client 最大运行时间（秒），默认 90
  --keep-logs              成功时也保留日志目录
EOF
}

die() {
    echo "$*" >&2
    exit 1
}

is_positive_int() {
    [[ "$1" =~ ^[0-9]+$ ]] && [ "$1" -gt 0 ]
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --duration-seconds)
                [ "$#" -ge 2 ] || die "缺少 --duration-seconds 参数值"
                DURATION_SECONDS="$2"
                shift 2
                ;;
            --clients)
                [ "$#" -ge 2 ] || die "缺少 --clients 参数值"
                NUM_CLIENTS="$2"
                shift 2
                ;;
            --bytes-per-client)
                [ "$#" -ge 2 ] || die "缺少 --bytes-per-client 参数值"
                BYTES_PER_CLIENT="$2"
                shift 2
                ;;
            --length)
                [ "$#" -ge 2 ] || die "缺少 --length 参数值"
                PAYLOAD_LENGTH="$2"
                shift 2
                ;;
            --report-interval)
                [ "$#" -ge 2 ] || die "缺少 --report-interval 参数值"
                REPORT_INTERVAL="$2"
                shift 2
                ;;
            --server-port)
                [ "$#" -ge 2 ] || die "缺少 --server-port 参数值"
                SERVER_PORT="$2"
                SERVER_PORT_AUTO=0
                shift 2
                ;;
            --rss-warn-threshold-kb)
                [ "$#" -ge 2 ] || die "缺少 --rss-warn-threshold-kb 参数值"
                RSS_WARN_THRESHOLD_KB="$2"
                shift 2
                ;;
            --rss-fail-threshold-kb)
                [ "$#" -ge 2 ] || die "缺少 --rss-fail-threshold-kb 参数值"
                RSS_FAIL_THRESHOLD_KB="$2"
                shift 2
                ;;
            --cpu-stop-threshold)
                [ "$#" -ge 2 ] || die "缺少 --cpu-stop-threshold 参数值"
                CPU_STOP_THRESHOLD="$2"
                shift 2
                ;;
            --client-timeout)
                [ "$#" -ge 2 ] || die "缺少 --client-timeout 参数值"
                CLIENT_TIMEOUT_SECONDS="$2"
                shift 2
                ;;
            --keep-logs)
                KEEP_LOGS=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                if [ -z "$BUILD_DIR" ]; then
                    BUILD_DIR="$1"
                    shift
                else
                    usage
                    die "未知参数: $1"
                fi
                ;;
        esac
    done

    [ -n "$BUILD_DIR" ] || {
        usage
        die "缺少 <build-dir>"
    }

    is_positive_int "$DURATION_SECONDS" || die "--duration-seconds 必须是正整数"
    is_positive_int "$NUM_CLIENTS" || die "--clients 必须是正整数"
    is_positive_int "$BYTES_PER_CLIENT" || die "--bytes-per-client 必须是正整数"
    is_positive_int "$PAYLOAD_LENGTH" || die "--length 必须是正整数"
    is_positive_int "$REPORT_INTERVAL" || die "--report-interval 必须是正整数"
    is_positive_int "$CLIENT_TIMEOUT_SECONDS" || die "--client-timeout 必须是正整数"
    [[ "$RSS_WARN_THRESHOLD_KB" =~ ^[0-9]+$ ]] || die "--rss-warn-threshold-kb 必须是非负整数"
    [[ "$RSS_FAIL_THRESHOLD_KB" =~ ^[0-9]+$ ]] || die "--rss-fail-threshold-kb 必须是非负整数"
    [[ "$CPU_STOP_THRESHOLD" =~ ^[0-9]+$ ]] || die "--cpu-stop-threshold 必须是非负整数"

    if [ "$SERVER_PORT_AUTO" -eq 0 ]; then
        is_positive_int "$SERVER_PORT" || die "--server-port 必须是正整数"
    fi
}

choose_server_port() {
    if [ "$SERVER_PORT_AUTO" -eq 0 ]; then
        return
    fi

    local seed
    seed=$(( (10#$(date +%S) * 1000000 + 10#$(date +%N) / 1000 + $$ + RANDOM) % 20000 ))
    SERVER_PORT=$((20000 + seed))
}

parse_done_result() {
    local output="$1"
    local sent_bytes done_bytes result

    sent_bytes=$(echo "$output" | sed -nE 's/.*sent_bytes=([0-9]+).*/\1/p' | tail -1)
    done_bytes=$(echo "$output" | sed -nE 's/.*done(_| )bytes=([0-9]+).*/\2/p' | tail -1)
    result=$(echo "$output" | sed -nE 's/.*result=([A-Z]+).*/\1/p' | tail -1)

    [ -n "$sent_bytes" ] || sent_bytes=0
    [ -n "$done_bytes" ] || done_bytes=0
    [ -n "$result" ] || result="UNKNOWN"

    echo "$sent_bytes $done_bytes $result"
}

get_rss_kb() {
    local pid="$1"
    awk '/VmRSS:/ {print $2; exit}' "/proc/$pid/status" 2>/dev/null || echo 0
}

get_fd_count() {
    local pid="$1"
    if [ -d "/proc/$pid/fd" ]; then
        ls "/proc/$pid/fd" 2>/dev/null | wc -l | tr -d ' '
    else
        echo 0
    fi
}

get_cpu_percent() {
    local pid="$1"
    ps -p "$pid" -o %cpu= 2>/dev/null | awk '{print $1 + 0}'
}

trigger_stop() {
    local reason="$1"
    echo "reason=$reason" >"$TMP_ROOT/.stop_reason"
    : >"$TMP_ROOT/.stop"
}

write_worker_state() {
    local file="$1"
    local success_rounds="$2"
    local failed_rounds="$3"
    local bytes_total="$4"
    local last_error="$5"

    cat >"$file" <<EOF
success_rounds=$success_rounds
failed_rounds=$failed_rounds
bytes_total=$bytes_total
last_error=$last_error
EOF
}

read_state_value() {
    local file="$1"
    local key="$2"
    [ -f "$file" ] || return 1
    sed -nE "s/^${key}=(.*)$/\\1/p" "$file" | tail -1
}

cleanup() {
    local pid
    for pid in "${WORKER_PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            pkill -P "$pid" 2>/dev/null || true
            kill "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${WORKER_PIDS[@]:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [ -n "$ECHO_CLIENT_BIN" ]; then
        pkill -f "${ECHO_CLIENT_BIN}.*--server-port ${SERVER_PORT}" 2>/dev/null || true
    fi

    if [ "$SERVER_PID" -ne 0 ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=0
    fi

    if [ "$KEEP_LOGS" -eq 0 ] && [ -d "$TMP_ROOT" ] && [ -f "$TMP_ROOT/.success" ]; then
        rm -rf "$TMP_ROOT"
    fi
}

on_interrupt() {
    echo ""
    echo "收到中断，停止测试..."
    exit 130
}

start_server() {
    local echo_server="$1"
    mkdir -p "$TMP_ROOT" "$CLIENT_LOG_DIR" "$CLIENT_STATE_DIR"
    : >"$SERVER_LOG"
    : >"$REPORT_FILE"

    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$echo_server" --bind-ip 127.0.0.1 --bind-port "$SERVER_PORT" >>"$SERVER_LOG" 2>&1 &
    else
        "$echo_server" --bind-ip 127.0.0.1 --bind-port "$SERVER_PORT" >>"$SERVER_LOG" 2>&1 &
    fi
    SERVER_PID=$!

    local timeout=16
    while [ "$timeout" -gt 0 ]; do
        if grep -q "listening" "$SERVER_LOG"; then
            SERVER_START_RSS_KB="$(get_rss_kb "$SERVER_PID")"
            SERVER_START_FD="$(get_fd_count "$SERVER_PID")"
            SERVER_PEAK_RSS_KB="$SERVER_START_RSS_KB"
            SERVER_PEAK_FD="$SERVER_START_FD"
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "  [ERROR] lsquic echo server exited before listening"
            tail -20 "$SERVER_LOG" | sed 's/^/    /'
            return 1
        fi
        sleep 0.5
        timeout=$((timeout - 1))
    done

    echo "  [ERROR] lsquic echo server failed to start (no listening log)"
    tail -20 "$SERVER_LOG" | sed 's/^/    /'
    return 1
}

run_worker() {
    local worker_id="$1"
    local end_epoch="$2"
    local echo_client="$3"
    local state_file="$CLIENT_STATE_DIR/client_${worker_id}.state"
    local log_file="$CLIENT_LOG_DIR/client_${worker_id}.log"

    local success_rounds=0
    local failed_rounds=0
    local bytes_total=0
    local last_error="NONE"

    : >"$log_file"
    write_worker_state "$state_file" "$success_rounds" "$failed_rounds" "$bytes_total" "$last_error"

    while [ "$(date +%s)" -lt "$end_epoch" ]; do
        if [ -f "$TMP_ROOT/.stop" ]; then
            return 0
        fi

        local output=""
        local client_status=0
        if output=$(timeout "$CLIENT_TIMEOUT_SECONDS" "$echo_client" \
            --server-ip 127.0.0.1 \
            --server-port "$SERVER_PORT" \
            --total-bytes "$BYTES_PER_CLIENT" \
            --chunk-bytes "$PAYLOAD_LENGTH" \
            --silent 2>&1); then
            client_status=0
        else
            client_status=$?
        fi

        local sent_bytes done_bytes result
        read -r sent_bytes done_bytes result < <(parse_done_result "$output")
        {
            echo "===== round $(date +%s) ====="
            echo "$output"
        } >>"$log_file"

        if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
           && [ "$sent_bytes" = "$BYTES_PER_CLIENT" ] && [ "$done_bytes" = "$BYTES_PER_CLIENT" ]; then
            success_rounds=$((success_rounds + 1))
            bytes_total=$((bytes_total + sent_bytes))
            last_error="NONE"
        else
            failed_rounds=$((failed_rounds + 1))
            if echo "$output" | grep -qi "connect"; then
                last_error="CONNECT_ERROR"
            elif echo "$output" | grep -qi "timeout"; then
                last_error="CLIENT_TIMEOUT"
            elif [ "$client_status" -eq 124 ]; then
                last_error="CLIENT_TIMEOUT"
            elif [ "$result" != "PASS" ]; then
                last_error="RESULT_${result}"
            else
                last_error="EXIT_${client_status}"
            fi
            write_worker_state "$state_file" "$success_rounds" "$failed_rounds" "$bytes_total" "$last_error"
            trigger_stop "WORKER_${worker_id}_${last_error}"
            return 1
        fi

        write_worker_state "$state_file" "$success_rounds" "$failed_rounds" "$bytes_total" "$last_error"
    done
}

main() {
    parse_args "$@"
    choose_server_port

    local echo_server="$BUILD_DIR/lsquic/lsquic_echo_server"
    local echo_client="$BUILD_DIR/lsquic/lsquic_echo_client"
    ECHO_CLIENT_BIN="$echo_client"

    [ -x "$echo_server" ] || die "找不到 $echo_server"
    [ -x "$echo_client" ] || die "找不到 $echo_client"
    [ -d /proc ] || die "当前环境缺少 /proc，无法采样 RSS/fd"

    trap cleanup EXIT
    trap on_interrupt INT TERM

    echo "========== lsquic 并发长稳定性测试 =========="
    echo "build_dir=$BUILD_DIR"
    echo "duration_seconds=$DURATION_SECONDS"
    echo "clients=$NUM_CLIENTS"
    echo "bytes_per_client=$BYTES_PER_CLIENT"
    echo "length=$PAYLOAD_LENGTH"
    echo "report_interval=$REPORT_INTERVAL"
    echo "server_port=$SERVER_PORT"
    echo "client_timeout_seconds=$CLIENT_TIMEOUT_SECONDS"
    echo "rss_warn_threshold_kb=$RSS_WARN_THRESHOLD_KB"
    echo "rss_fail_threshold_kb=$RSS_FAIL_THRESHOLD_KB"
    echo "cpu_stop_threshold=$CPU_STOP_THRESHOLD"
    echo "tmp_root=$TMP_ROOT"
    echo ""

    start_server "$echo_server" || exit 1

    local end_epoch
    end_epoch="$(($(date +%s) + DURATION_SECONDS))"
    local i
    for i in $(seq 1 "$NUM_CLIENTS"); do
        run_worker "$i" "$end_epoch" "$echo_client" &
        WORKER_PIDS+=($!)
    done

    local total_success=0
    local total_failed=0
    local total_bytes=0
    local alive_workers=0
    local elapsed=0
    local stop_reason="NONE"

    while [ "$(date +%s)" -lt "$end_epoch" ]; do
        if [ -f "$TMP_ROOT/.stop" ]; then
            stop_reason="$(read_state_value "$TMP_ROOT/.stop_reason" reason || echo STOP)"
            break
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            SERVER_EXITED_EARLY=1
            stop_reason="SERVER_EXITED"
            break
        fi

        total_success=0
        total_failed=0
        total_bytes=0
        alive_workers=0

        for i in $(seq 1 "$NUM_CLIENTS"); do
            local state_file="$CLIENT_STATE_DIR/client_${i}.state"
            local s f b
            s="$(read_state_value "$state_file" success_rounds || echo 0)"
            f="$(read_state_value "$state_file" failed_rounds || echo 0)"
            b="$(read_state_value "$state_file" bytes_total || echo 0)"
            total_success=$((total_success + s))
            total_failed=$((total_failed + f))
            total_bytes=$((total_bytes + b))
        done

        for pid in "${WORKER_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                alive_workers=$((alive_workers + 1))
            fi
        done

        local rss_kb fd_count cpu_percent
        rss_kb="$(get_rss_kb "$SERVER_PID")"
        fd_count="$(get_fd_count "$SERVER_PID")"
        cpu_percent="$(get_cpu_percent "$SERVER_PID")"
        [ "$rss_kb" -gt "$SERVER_PEAK_RSS_KB" ] && SERVER_PEAK_RSS_KB="$rss_kb"
        [ "$fd_count" -gt "$SERVER_PEAK_FD" ] && SERVER_PEAK_FD="$fd_count"

        elapsed=$((DURATION_SECONDS - (end_epoch - $(date +%s))))
        printf "elapsed_s=%s success_rounds=%s failed_rounds=%s bytes_total=%s server_rss_kb=%s server_fd_count=%s server_cpu_percent=%s active_clients=%s\n" \
            "$elapsed" "$total_success" "$total_failed" "$total_bytes" "$rss_kb" "$fd_count" "$cpu_percent" "$alive_workers" \
            | tee -a "$REPORT_FILE"

        if [ "$CPU_STOP_THRESHOLD" -gt 0 ]; then
            if awk "BEGIN {exit !($cpu_percent > $CPU_STOP_THRESHOLD)}"; then
                echo "  [ERROR] server CPU ${cpu_percent}% 超过阈值 ${CPU_STOP_THRESHOLD}%"
                stop_reason="CPU_THRESHOLD"
                trigger_stop "$stop_reason"
                break
            fi
        fi

        sleep "$REPORT_INTERVAL"
    done

    for pid in "${WORKER_PIDS[@]}"; do
        wait "$pid" || true
    done

    total_success=0
    total_failed=0
    total_bytes=0
    local connect_error_count=0
    local result_fail_count=0
    local other_fail_count=0
    for i in $(seq 1 "$NUM_CLIENTS"); do
        local state_file="$CLIENT_STATE_DIR/client_${i}.state"
        local s f b e
        s="$(read_state_value "$state_file" success_rounds || echo 0)"
        f="$(read_state_value "$state_file" failed_rounds || echo 0)"
        b="$(read_state_value "$state_file" bytes_total || echo 0)"
        e="$(read_state_value "$state_file" last_error || echo NONE)"
        total_success=$((total_success + s))
        total_failed=$((total_failed + f))
        total_bytes=$((total_bytes + b))
        case "$e" in
            CONNECT_ERROR) connect_error_count=$((connect_error_count + 1)) ;;
            RESULT_*) result_fail_count=$((result_fail_count + 1)) ;;
            NONE) ;;
            *) other_fail_count=$((other_fail_count + 1)) ;;
        esac
    done

    local server_end_rss_kb=0
    local server_end_fd=0
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        server_end_rss_kb="$(get_rss_kb "$SERVER_PID")"
        server_end_fd="$(get_fd_count "$SERVER_PID")"
    fi

    echo ""
    echo "=========================================="
    echo "duration_seconds=$DURATION_SECONDS"
    echo "clients=$NUM_CLIENTS"
    echo "success_rounds=$total_success"
    echo "failed_rounds=$total_failed"
    echo "bytes_total=$total_bytes"
    echo "server_rss_start_kb=$SERVER_START_RSS_KB"
    echo "server_rss_end_kb=$server_end_rss_kb"
    echo "server_rss_peak_kb=$SERVER_PEAK_RSS_KB"
    echo "server_fd_start=$SERVER_START_FD"
    echo "server_fd_end=$server_end_fd"
    echo "server_fd_peak=$SERVER_PEAK_FD"
    echo "stop_reason=$stop_reason"
    echo "connect_error_count=$connect_error_count"
    echo "result_fail_count=$result_fail_count"
    echo "other_fail_count=$other_fail_count"
    echo "server_log=$SERVER_LOG"
    echo "client_logs=$CLIENT_LOG_DIR"
    echo "report_log=$REPORT_FILE"

    local status=0
    if [ "$SERVER_EXITED_EARLY" -ne 0 ]; then
        echo "FAIL: server exited early"
        status=1
    elif [ "$total_failed" -ne 0 ]; then
        echo "FAIL: observed failed rounds"
        status=1
    elif [ "$server_end_fd" -gt $((SERVER_START_FD + 3)) ]; then
        echo "FAIL: server fd count grew unexpectedly"
        status=1
    elif [ "$RSS_FAIL_THRESHOLD_KB" -gt 0 ] && [ "$server_end_rss_kb" -gt $((SERVER_START_RSS_KB + RSS_FAIL_THRESHOLD_KB)) ]; then
        echo "FAIL: server RSS exceeded fail threshold"
        status=1
    else
        if [ "$server_end_rss_kb" -gt $((SERVER_START_RSS_KB + RSS_WARN_THRESHOLD_KB)) ]; then
            echo "WARN: server RSS exceeded warning threshold"
        fi
        : >"$TMP_ROOT/.success"
        echo "PASS"
        status=0
    fi

    exit "$status"
}

main "$@"
