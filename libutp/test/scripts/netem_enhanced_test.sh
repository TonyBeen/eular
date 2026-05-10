#!/usr/bin/env bash
# netem_enhanced_test.sh — libutp 增强网络仿真测试（大流量、突发丢包、并发）
# 用法: sudo ./test/scripts/netem_enhanced_test.sh <build-dir> [--allow-no-root]

set -euo pipefail

BUILD_DIR=""
ALLOW_NO_ROOT="0"

for arg in "$@"; do
    case "$arg" in
        --allow-no-root)
            ALLOW_NO_ROOT="1"
            ;;
        *)
            if [ -z "$BUILD_DIR" ]; then
                BUILD_DIR="$arg"
            else
                echo "用法: sudo ./test/scripts/netem_enhanced_test.sh <build-dir> [--allow-no-root]" >&2
                exit 1
            fi
            ;;
    esac
done

if [ -z "$BUILD_DIR" ]; then
    echo "用法: sudo ./test/scripts/netem_enhanced_test.sh <build-dir> [--allow-no-root]" >&2
    exit 1
fi

ECHO_SERVER="$BUILD_DIR/examples/utp_echo_server"
ECHO_CLIENT="$BUILD_DIR/examples/utp_echo_client"
IFACE="lo"
ECHO_PORT=9100
ECHO_SERVER_LOG="$(mktemp /tmp/utp_netem_echo_server.XXXXXX.log)"
ECHO_SERVER_PID=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass=0; fail=0

netem_set() {
    if [ "$ALLOW_NO_ROOT" = "1" ]; then
        return 0
    fi
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    [ -n "$1" ] && tc qdisc add dev "$IFACE" root netem $1 || true
}

netem_clear() {
    if [ "$ALLOW_NO_ROOT" = "1" ]; then
        return 0
    fi
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
}

start_echo_server() {
    > "$ECHO_SERVER_LOG"
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$ECHO_SERVER" --bind-ip 127.0.0.1 --bind-port "$ECHO_PORT" >>"$ECHO_SERVER_LOG" 2>&1 &
    else
        "$ECHO_SERVER" --bind-ip 127.0.0.1 --bind-port "$ECHO_PORT" >>"$ECHO_SERVER_LOG" 2>&1 &
    fi
    ECHO_SERVER_PID=$!
    local timeout=8
    while [ $timeout -gt 0 ]; do
        if grep -q "listening" "$ECHO_SERVER_LOG"; then
            sleep 1
            return 0
        fi
        if ! kill -0 "$ECHO_SERVER_PID" 2>/dev/null; then
            echo "  [ERROR] echo server exited before listening"
            tail -20 "$ECHO_SERVER_LOG" | sed 's/^/    /'
            stop_echo_server
            return 1
        fi
        sleep 0.5
        ((timeout--)) || true
    done
    echo "  [ERROR] echo server failed to start (no listening log)"
    tail -20 "$ECHO_SERVER_LOG" | sed 's/^/    /'
    stop_echo_server
    return 1
}

stop_echo_server() {
    [ "$ECHO_SERVER_PID" -ne 0 ] && kill "$ECHO_SERVER_PID" 2>/dev/null || true
    wait "$ECHO_SERVER_PID" 2>/dev/null || true
    ECHO_SERVER_PID=0
}

cleanup() {
    stop_echo_server
    netem_clear
}

on_interrupt() {
    cleanup
    exit 130
}

parse_done_result() {
    local output="$1"
    local sent_bytes done_bytes result

    sent_bytes=$(echo "$output" | sed -nE 's/.*sent_bytes=([0-9]+).*/\1/p' | tail -1)
    done_bytes=$(echo "$output" | sed -nE 's/.*done bytes=([0-9]+).*/\1/p' | tail -1)
    result=$(echo "$output" | sed -nE 's/.*result=([A-Z]+).*/\1/p' | tail -1)

    [ -n "$sent_bytes" ] || sent_bytes=0
    [ -n "$done_bytes" ] || done_bytes=0
    [ -n "$result" ] || result="UNKNOWN"

    echo "$sent_bytes $done_bytes $result"
}

run_echo_scenario() {
    local name="$1"
    local netem_args="$2"
    local count="${3:-10}"
    local timeout_s="${4:-30}"
    local expected_bytes=$((count * 64))

    echo -e "\n${YELLOW}>>> $name${NC}"
    [ -n "$netem_args" ] && echo "  netem: $netem_args" || echo "  netem: (none)"

    netem_set "$netem_args"
    if ! start_echo_server; then
        netem_clear
        ((fail++)) || true
        return
    fi

    local output=""
    local client_status=0
    if ! output=$(timeout "$timeout_s" "$ECHO_CLIENT" \
        --server-ip 127.0.0.1 --server-port "$ECHO_PORT" \
        --count "$count" --length 64 2>&1); then
        client_status=$?
    fi

    stop_echo_server
    netem_clear

    local sent_bytes echoed_bytes result
    read -r sent_bytes echoed_bytes result < <(parse_done_result "$output")

    if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
       && [ "$sent_bytes" = "$expected_bytes" ] && [ "$echoed_bytes" = "$expected_bytes" ]; then
        echo -e "  ${GREEN}PASS${NC}  expected=$expected_bytes sent=$sent_bytes done=$echoed_bytes"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  expected=$expected_bytes sent=$sent_bytes done=$echoed_bytes result=$result status=$client_status"
        echo "$output" | tail -20 | sed 's/^/    /'
        ((fail++)) || true
    fi
}

run_burst_loss_scenario() {
    local name="$1"
    local gemodel_args="$2"
    local count="${3:-10}"
    local timeout_s="${4:-35}"
    local expected_bytes=$((count * 64))

    echo -e "\n${YELLOW}>>> $name${NC}"
    echo "  netem: loss gemodel $gemodel_args"

    netem_set "loss gemodel $gemodel_args"
    if ! start_echo_server; then
        netem_clear
        ((fail++)) || true
        return
    fi

    local output=""
    local client_status=0
    if ! output=$(timeout "$timeout_s" "$ECHO_CLIENT" \
        --server-ip 127.0.0.1 --server-port "$ECHO_PORT" \
        --count "$count" --length 64 2>&1); then
        client_status=$?
    fi

    stop_echo_server
    netem_clear

    local sent_bytes echoed_bytes result
    read -r sent_bytes echoed_bytes result < <(parse_done_result "$output")

    if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
       && [ "$sent_bytes" = "$expected_bytes" ] && [ "$echoed_bytes" = "$expected_bytes" ]; then
        echo -e "  ${GREEN}PASS${NC}  expected=$expected_bytes sent=$sent_bytes done=$echoed_bytes"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  expected=$expected_bytes sent=$sent_bytes done=$echoed_bytes result=$result status=$client_status"
        echo "$output" | tail -20 | sed 's/^/    /'
        ((fail++)) || true
    fi
}

run_large_transfer_scenario() {
    local name="$1"
    local netem_args="$2"
    local total_bytes="$3"
    local length="$4"
    local timeout_s="${5:-240}"

    echo -e "\n${YELLOW}>>> $name${NC}"
    [ -n "$netem_args" ] && echo "  netem: $netem_args" || echo "  netem: (none)"
    echo "  total_bytes=$total_bytes length=$length"

    netem_set "$netem_args"
    if ! start_echo_server; then
        netem_clear
        ((fail++)) || true
        return
    fi

    local output=""
    local client_status=0
    if ! output=$(timeout "$timeout_s" "$ECHO_CLIENT" \
        --server-ip 127.0.0.1 --server-port "$ECHO_PORT" \
        --length "$length" --count 200000 \
        --total-bytes "$total_bytes" --quiet 2>&1); then
        client_status=$?
    fi

    stop_echo_server
    netem_clear

    local sent_bytes echoed_bytes result
    read -r sent_bytes echoed_bytes result < <(parse_done_result "$output")

    if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
       && [ "$sent_bytes" = "$total_bytes" ] && [ "$echoed_bytes" = "$total_bytes" ]; then
        echo -e "  ${GREEN}PASS${NC}  100MB 级别传输完成，bytes=$sent_bytes"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  expected=$total_bytes sent=$sent_bytes done=$echoed_bytes result=$result status=$client_status"
        echo "$output" | tail -30 | sed 's/^/    /'
        ((fail++)) || true
    fi
}

run_bulk_concurrent_scenario() {
    local name="$1"
    local netem_args="$2"
    local num_clients="$3"
    local bytes_per_client="$4"
    local length="$5"
    local timeout_s="${6:-240}"

    echo -e "\n${YELLOW}>>> $name${NC}"
    [ -n "$netem_args" ] && echo "  netem: $netem_args" || echo "  netem: (none)"
    echo "  clients=$num_clients bytes_per_client=$bytes_per_client length=$length"

    netem_set "$netem_args"
    if ! start_echo_server; then
        netem_clear
        ((fail++)) || true
        return
    fi

    local pids=()
    local success_count=0

    for i in $(seq 1 "$num_clients"); do
        (
            local output=""
            local client_status=0
            if ! output=$(timeout "$timeout_s" "$ECHO_CLIENT" \
                --server-ip 127.0.0.1 --server-port "$ECHO_PORT" \
                --length "$length" --count 200000 \
                --total-bytes "$bytes_per_client" --quiet 2>&1); then
                client_status=$?
            fi

            local sb eb result
            read -r sb eb result < <(parse_done_result "$output")
            if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
               && [ "$sb" = "$bytes_per_client" ] && [ "$eb" = "$bytes_per_client" ]; then
                echo "    [客户端 $i] PASS bytes=$sb"
                exit 0
            else
                echo "    [客户端 $i] FAIL sent=$sb done=$eb result=$result status=$client_status"
                exit 1
            fi
        ) &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        if wait "$pid" 2>/dev/null; then
            ((success_count++)) || true
        fi
    done

    stop_echo_server
    netem_clear

    local success_rate=$(( success_count * 100 / num_clients ))
    if [ "$success_count" = "$num_clients" ]; then
        echo -e "  ${GREEN}PASS${NC}  并发大流量成功率 ${success_rate}%"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  并发大流量成功率 ${success_rate}% ($success_count/$num_clients)"
        ((fail++)) || true
    fi
}

if [ "$(id -u)" != "0" ]; then
    if [ "$ALLOW_NO_ROOT" != "1" ]; then
        echo "需要 root 权限操作 tc qdisc，请用 sudo 运行，或设置 ALLOW_NO_ROOT=1 跳过 netem" >&2
        exit 1
    fi
fi

trap cleanup EXIT
trap on_interrupt INT TERM

for bin in "$ECHO_SERVER" "$ECHO_CLIENT"; do
    [ -x "$bin" ] || { echo "找不到 $bin" >&2; exit 1; }
done

echo "========== libutp 增强网络仿真测试 =========="
echo ""

echo "【Phase 3 回归】基础网络仿真（8 场景）"
run_echo_scenario "场景1: 基准（无损）"           ""                                   10 20
run_echo_scenario "场景2: 丢包 1%"               "loss 1%"                            10 30
run_echo_scenario "场景3: 丢包 5%"               "loss 5%"                            10 40
run_echo_scenario "场景4: 延迟 50ms"             "delay 50ms"                         10 25
run_echo_scenario "场景5: 延迟 200ms"            "delay 200ms"                        5  40
run_echo_scenario "场景6: 乱序 10%"              "delay 5ms reorder 10% 50%"          10 30
run_echo_scenario "场景7: 抖动 20ms±10ms"        "delay 20ms 10ms"                    10 30
run_echo_scenario "场景8: 综合（丢包+延迟+乱序）" "loss 1% delay 30ms reorder 5% 50%" 10 40

echo ""
echo "【Phase 4a】大文件流测试"
run_large_transfer_scenario "A1: 单连接 100MB 无损" "" 104857600 4096 240
run_large_transfer_scenario "A2: 单连接 100MB + 1% 丢包" "loss 1%" 104857600 4096 300

echo ""
echo "【Phase 4b】突发丢包（Gemodel）"
run_burst_loss_scenario "B1: 轻度突发丢包" "10% 80% 10% 10%" 10 35
run_burst_loss_scenario "B2: 中度突发丢包" "20% 70% 15% 20%" 10 40
run_burst_loss_scenario "B3: 重度突发丢包" "30% 60% 20% 30%" 10 50

echo ""
echo "【Phase 4c】并发大流量"
# 10 客户端 * 10MB = 总 100MB
run_bulk_concurrent_scenario "C1: 10 并发，总量 100MB，无损" "" 10 10485760 2048 240
run_bulk_concurrent_scenario "C2: 10 并发，总量 100MB，1% 丢包" "loss 1%" 10 10485760 2048 300

echo ""
echo "=========================================="
echo -e "  结果: ${GREEN}PASS $pass${NC}  ${RED}FAIL $fail${NC}"
echo "=========================================="
[ "$fail" -eq 0 ] && exit 0 || exit 1
