#!/usr/bin/env bash
# netem_test.sh — libutp 网络仿真测试脚本
# 用法: sudo ./test/scripts/netem_test.sh <build-dir>
#
# 验证方式：
#   基于当前 utp_echo_client/utp_echo_server 的文本协议输出进行校验：
#   - client 侧必须成功发送预期字节数
#   - server 侧必须回 DONE，且 done bytes 与 sent_bytes 一致
#   - client 最终 result 必须为 PASS
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "用法: sudo ./test/scripts/netem_test.sh <build-dir>" >&2
    exit 1
fi

BUILD_DIR="$1"
SERVER="$BUILD_DIR/examples/utp_echo_server"
CLIENT="$BUILD_DIR/examples/utp_echo_client"
IFACE="lo"
PORT=9100
SERVER_LOG="/tmp/utp_netem_server.log"
SERVER_PID=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass=0; fail=0

netem_set() {
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    [ -n "$1" ] && tc qdisc add dev "$IFACE" root netem $1 || true
}

netem_clear() {
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
}

start_server() {
    > "$SERVER_LOG"
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$SERVER" --bind-ip 127.0.0.1 --bind-port "$PORT" >>"$SERVER_LOG" 2>&1 &
    else
        "$SERVER" --bind-ip 127.0.0.1 --bind-port "$PORT" >>"$SERVER_LOG" 2>&1 &
    fi
    SERVER_PID=$!

    local timeout=8
    while [ $timeout -gt 0 ]; do
        if grep -q "listening" "$SERVER_LOG"; then
            sleep 1
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "  [ERROR] echo server exited before listening"
            tail -20 "$SERVER_LOG" | sed 's/^/    /'
            stop_server
            return 1
        fi
        sleep 0.5
        ((timeout--)) || true
    done
    echo "  [ERROR] echo server failed to start (no listening log)"
    tail -20 "$SERVER_LOG" | sed 's/^/    /'
    stop_server
    return 1
}

stop_server() {
    [ "$SERVER_PID" -ne 0 ] && kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=0
}

run_scenario() {
    local name="$1"
    local netem_args="$2"
    local count="${3:-10}"
    local timeout_s="${4:-30}"
    local expected_bytes=$((count * 64))

    echo -e "\n${YELLOW}>>> $name${NC}"
    [ -n "$netem_args" ] && echo "  netem: $netem_args" || echo "  netem: (none)"

    netem_set "$netem_args"
    if ! start_server; then
        netem_clear
        ((fail++)) || true
        return
    fi

    local output=""
    local client_status=0
    if ! output=$(timeout "$timeout_s" "$CLIENT" \
        --server-ip 127.0.0.1 --server-port "$PORT" \
        --count "$count" --length 64 2>&1); then
        client_status=$?
    fi

    stop_server
    netem_clear

    local sent_bytes done_bytes result
    sent_bytes=$(echo "$output" | sed -nE 's/.*sent_bytes=([0-9]+).*/\1/p' | tail -1)
    done_bytes=$(echo "$output" | sed -nE 's/.*done bytes=([0-9]+).*/\1/p' | tail -1)
    result=$(echo "$output" | sed -nE 's/.*result=([A-Z]+).*/\1/p' | tail -1)

    [ -n "$sent_bytes" ] || sent_bytes=0
    [ -n "$done_bytes" ] || done_bytes=0
    [ -n "$result" ] || result="UNKNOWN"

    if [ "$client_status" -eq 0 ] && [ "$result" = "PASS" ] \
       && [ "$sent_bytes" = "$expected_bytes" ] && [ "$done_bytes" = "$expected_bytes" ]; then
        echo -e "  ${GREEN}PASS${NC}  expected=$expected_bytes sent=$sent_bytes done=$done_bytes"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  expected=$expected_bytes sent=$sent_bytes done=$done_bytes result=$result status=$client_status"
        echo "  --- client output ---"
        echo "$output" | sed 's/^/    /'
        ((fail++)) || true
    fi
}

# 检查权限与二进制
if [ "$(id -u)" != "0" ]; then
    echo "需要 root 权限操作 tc qdisc，请用 sudo 运行" >&2; exit 1
fi
for bin in "$SERVER" "$CLIENT"; do
    [ -x "$bin" ] || { echo "找不到 $bin" >&2; exit 1; }
done

echo "=== libutp netem 网络仿真测试 ==="

run_scenario "场景1: 基准（无损）"           ""                                   10 20
run_scenario "场景2: 丢包 1%"               "loss 1%"                            10 30
run_scenario "场景3: 丢包 5%"               "loss 5%"                            10 40
run_scenario "场景4: 延迟 50ms"             "delay 50ms"                         10 25
run_scenario "场景5: 延迟 200ms"            "delay 200ms"                        5  40
run_scenario "场景6: 乱序 10%"              "delay 5ms reorder 10% 50%"          10 30
run_scenario "场景7: 抖动 20ms±10ms"        "delay 20ms 10ms"                    10 30
run_scenario "场景8: 综合（丢包+延迟+乱序）" "loss 1% delay 30ms reorder 5% 50%" 10 40

echo ""
echo "===================================="
echo -e "  结果: ${GREEN}PASS $pass${NC}  ${RED}FAIL $fail${NC}"
echo "===================================="
[ "$fail" -eq 0 ] && exit 0 || exit 1
