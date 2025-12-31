#########################################################################
# File Name: kcp_bench.sh
# Author: hsz
# brief:
# Created Time: 2025年11月06日 星期四 11时38分06秒
#########################################################################
#!/bin/bash

#!/usr/bin/env bash
# 持续压测 kcp_file_server，保持固定并发数
# 用法: ./kcp_stress_test.sh 5 -s www.eular.top -f data.tar.gz

usage() {
    echo "用法: $0 <并发数> <kcp_sendfile参数>"
    echo "示例: $0 5 -s www.eular.top -f data.tar.gz"
    exit 1
}

if [ $# -lt 2 ]; then
    usage
fi

CONCURRENCY=$1
shift

SEND_FILE_BIN="./kcp_sendfile.out"
if [ ! -x "$SEND_FILE_BIN" ]; then
    echo "错误: $SEND_FILE_BIN 不存在或不可执行"
    exit 2
fi

echo "维持并发数: $CONCURRENCY"
echo "kcp_sendfile 参数: $@"
echo "按 Ctrl+C 终止测试"
echo "-------------------------"

# 存储 PID 与启动次数的关联
declare -A PID_MAP

# 启动一个新进程
start_job() {
    local job_id=$1
    $SEND_FILE_BIN "$@" &
    local pid=$!
    PID_MAP[$job_id]=$pid
    echo "[启动] Job#$job_id => PID $pid"
}

# 杀掉所有子进程
cleanup() {
    echo -e "\n[清理] 终止所有进程..."
    for pid in "${PID_MAP[@]}"; do
        kill "$pid" 2>/dev/null
    done
    exit 0
}

trap cleanup SIGINT SIGTERM

# 启动初始进程
for ((i=1; i<=CONCURRENCY; i++)); do
    start_job $i "$@"
done

# 主循环：监控进程
while true; do
    for job_id in "${!PID_MAP[@]}"; do
        pid=${PID_MAP[$job_id]}
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[检测] Job#$job_id (PID $pid) 已退出, 1 秒后重启..."
            sleep 1
            start_job $job_id "$@"
        fi
    done
    sleep 0.5  # 降低 CPU 占用
done
