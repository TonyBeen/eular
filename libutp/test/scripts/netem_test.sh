#!/usr/bin/env bash
# netem_test.sh — libutp 网络仿真测试脚本
# 用法: sudo ./test/scripts/netem_test.sh [build-dir]
#
# 验证方式（严格）：
#   提取每条 "[client] send #N: "payload"" 的 payload 字符串，
#   把所有 echo 回来的 payload 拼接成一个大字符串（echo_blob），
#   逐一检查每条 sent-payload 是否出现在 echo_blob 中（substring 匹配）。
#   字节流粘包时 server 可能把多条消息合并一次 echo，substring 匹配仍能正确通过。
#   只有 payload 真正丢失时才 FAIL。
set -euo pipefail

BUILD_DIR="${1:-/home/eular/VSCode/eular/libutp/build-fi}"
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
    "$SERVER" --bind-ip 127.0.0.1 --bind-port "$PORT" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 0.5
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
    local interval_ms="${4:-500}"
    local timeout_s="${5:-30}"

    echo -e "\n${YELLOW}>>> $name${NC}"
    [ -n "$netem_args" ] && echo "  netem: $netem_args" || echo "  netem: (none)"

    netem_set "$netem_args"
    start_server

    local output=""
    output=$(timeout "$timeout_s" "$CLIENT" \
        --server-ip 127.0.0.1 --server-port "$PORT" \
        --count "$count" --interval-ms "$interval_ms" --length 64 2>&1) || true

    stop_server
    netem_clear

    # 提取 sent_count
    local sent_count
    sent_count=$(echo "$output" | grep -oP 'sent=\K[0-9]+' | tail -1 || echo 0)

    # 把所有 echo payload 拼成一个大字符串
    local echo_blob
    echo_blob=$(echo "$output" \
        | grep -oP '\[client\] echo #[0-9]+: "\K[^"]+' \
        | tr -d '\n')

    # 逐条检查每条 sent payload 是否出现在 echo_blob 中
    local missing=0 bad_payloads=""
    while IFS= read -r payload; do
        if [[ "$echo_blob" != *"$payload"* ]]; then
            missing=$(( missing + 1 ))
            bad_payloads="${bad_payloads}\n    MISSING: ${payload:0:32}..."
        fi
    done < <(echo "$output" | grep -oP '\[client\] send #[0-9]+: "\K[^"]+')

    if [ "$sent_count" = "$count" ] && [ "$missing" -eq 0 ]; then
        echo -e "  ${GREEN}PASS${NC}  sent=$sent_count，所有 $count 条 payload 均在 echo 中找到"
        ((pass++)) || true
    else
        echo -e "  ${RED}FAIL${NC}  sent=$sent_count/$count，缺失 $missing 条 payload"
        [ -n "$bad_payloads" ] && echo -e "$bad_payloads"
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

run_scenario "场景1: 基准（无损）"           ""                                   10 200 20
run_scenario "场景2: 丢包 1%"               "loss 1%"                            10 500 30
run_scenario "场景3: 丢包 5%"               "loss 5%"                            10 800 40
run_scenario "场景4: 延迟 50ms"             "delay 50ms"                         10 300 25
run_scenario "场景5: 延迟 200ms"            "delay 200ms"                        5  800 40
run_scenario "场景6: 乱序 10%"              "delay 5ms reorder 10% 50%"          10 400 30
run_scenario "场景7: 抖动 20ms±10ms"        "delay 20ms 10ms"                    10 400 30
run_scenario "场景8: 综合（丢包+延迟+乱序）" "loss 1% delay 30ms reorder 5% 50%" 10 600 40

echo ""
echo "===================================="
echo -e "  结果: ${GREEN}PASS $pass${NC}  ${RED}FAIL $fail${NC}"
echo "===================================="
[ "$fail" -eq 0 ] && exit 0 || exit 1
