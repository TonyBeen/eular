# Notify Hotpath Perf Report

测试时间：2026-06-26 18:04:51 CST

## 测试设备

| 项目 | 值 |
|---|---|
| 设备 | Mac mini |
| 型号 | Mac16,10 |
| 芯片 | Apple M4 |
| CPU 核心 | 10 核，4 性能核 + 6 能效核 |
| 内存 | 16 GB |
| 系统 | macOS 26.5.1，Build 25F80 |
| Kernel | Darwin 25.5.0 arm64 |
| 编译器 | Apple clang 17.0.0 (clang-1700.6.3.2) |
| 构建目录 | `/Users/eular/VSCode/eular/libevent/build` |

## 测试命令

构建：

```bash
cmake --build build --target test_event_async_perf test_single_async_perf -j4
```

测试并发线程数：

```text
1 / 2 / 4 / 8 / 16 / 24 / 32
```

EventAsync：

```bash
./test/test_event_async_perf --producers <N> --total 4096 --ids 64 --drain-ms 500 --warmup-ms 10
```

SingleAsync：

```bash
./test/test_single_async_perf --producers <N> --total 4096 --drain-ms 500 --warmup-ms 10
```

## 测试参数

| 参数 | 值 |
|---|---:|
| 每组总 notify 次数 | 4096 |
| EventAsync ID 数量 | 64 |
| warmup | 10 ms |
| drain | 500 ms |
| 每个线程数轮次 | 1 |
| 统计指标 | `avg_notify_ns = send_elapsed_sec * 1e9 / notify_success` |

## 测试结果

| 并发线程 | EventAsync notify_success | EventAsync callback_count | EventAsync avg_notify_ns | SingleAsync notify_success | SingleAsync callback_count | SingleAsync coalesced_rate_pct | SingleAsync avg_notify_ns |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4096 | 4096 | 297.292 | 4096 | 13 | 99.6826 | 276.937 |
| 2 | 4096 | 4096 | 355.560 | 4096 | 20 | 99.5117 | 421.427 |
| 4 | 4096 | 4096 | 796.488 | 4096 | 46 | 98.8770 | 1036.600 |
| 8 | 4096 | 4096 | 4432.760 | 4096 | 235 | 94.2627 | 5550.060 |
| 16 | 4096 | 4096 | 6252.230 | 4096 | 356 | 91.3086 | 8735.110 |
| 24 | 4096 | 4096 | 6742.270 | 4096 | 316 | 92.2852 | 8964.110 |
| 32 | 4096 | 4096 | 7236.630 | 4096 | 294 | 92.8223 | 9282.090 |

## 吞吐数据

| 并发线程 | EventAsync notify_qps | EventAsync qps/thread | SingleAsync notify_qps | SingleAsync qps/thread |
|---:|---:|---:|---:|---:|
| 1 | 3363700 | 3363700 | 3610930 | 3610930 |
| 2 | 2812460 | 1406230 | 2372890 | 1186440 |
| 4 | 1255510 | 313878 | 964691 | 241173 |
| 8 | 225593 | 28199.1 | 180178 | 22522.3 |
| 16 | 159943 | 9996.44 | 114481 | 7155.04 |
| 24 | 148318 | 6179.92 | 111556 | 4648.16 |
| 32 | 138186 | 4318.31 | 107734 | 3366.7 |

## 原始命令输出摘要

EventAsync 在本轮中所有并发下 `callback_count == notify_success == 4096`。

SingleAsync 是单回调聚合模型，本轮 callback 数如下：

| 并发线程 | callback_count | coalesced | coalesced_rate_pct |
|---:|---:|---:|---:|
| 1 | 13 | 4083 | 99.6826 |
| 2 | 20 | 4076 | 99.5117 |
| 4 | 46 | 4050 | 98.8770 |
| 8 | 235 | 3861 | 94.2627 |
| 16 | 356 | 3740 | 91.3086 |
| 24 | 316 | 3780 | 92.2852 |
| 32 | 294 | 3802 | 92.8223 |

## Linux 测试结果

测试时间：2026-06-26 18:12:05 CST

### 测试设备

| 项目 | 值 |
|---|---|
| 设备 | UM480XT |
| CPU | AMD Ryzen 7 4800H with Radeon Graphics |
| CPU 核心 | 8 核 / 16 线程 |
| 内存 | 15 GiB |
| 系统 | Ubuntu 20.04.6 LTS |
| Kernel | Linux 5.15.0-139-generic x86_64 |
| 编译器 | GNU g++ 11.2.1 (`/opt/musl-toolchain/bin/x86_64-linux-musl-g++`) |
| 构建类型 | RelWithDebInfo (`-O2 -g -DNDEBUG`) |
| 构建目录 | `/home/eular/VSCode/eular/libevent/build-musl` |

### 测试命令

构建：

```bash
cmake --build build-musl --target test_event_async_perf test_single_async_perf -j4
```

测试并发线程数：

```text
1 / 2 / 4 / 8 / 16 / 24 / 32
```

EventAsync：

```bash
./build-musl/test/test_event_async_perf --producers <N> --total 4096 --ids 64 --drain-ms 500 --warmup-ms 10
```

SingleAsync：

```bash
./build-musl/test/test_single_async_perf --producers <N> --total 4096 --drain-ms 500 --warmup-ms 10
```

### 测试参数

| 参数 | 值 |
|---|---:|
| 每组总 notify 次数 | 4096 |
| EventAsync ID 数量 | 64 |
| warmup | 10 ms |
| drain | 500 ms |
| 每个线程数轮次 | 1 |
| 统计指标 | `avg_notify_ns = send_elapsed_sec * 1e9 / notify_success` |

### 测试结果

Linux 本轮按测试程序输出记录成功通知数，多线程下部分 `notify()` 调用未成功，吞吐与平均耗时均基于 `notify_success`。

| 并发线程 | EventAsync notify_success | EventAsync callback_count | EventAsync avg_notify_ns | SingleAsync notify_success | SingleAsync callback_count | SingleAsync coalesced_rate_pct | SingleAsync avg_notify_ns |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4096 | 4096 | 1345.860 | 4096 | 24 | 99.4141 | 1924.870 |
| 2 | 3977 | 3977 | 1660.990 | 2149 | 1 | 99.9535 | 1106.100 |
| 4 | 1522 | 1522 | 1634.920 | 1398 | 2 | 99.8569 | 1380.630 |
| 8 | 1233 | 1233 | 1460.640 | 919 | 1 | 99.8912 | 1388.610 |
| 16 | 1430 | 1430 | 884.777 | 813 | 2 | 99.7540 | 1000.900 |
| 24 | 1404 | 1404 | 851.363 | 842 | 1 | 99.8812 | 944.776 |
| 32 | 1435 | 1435 | 871.277 | 794 | 1 | 99.8741 | 862.107 |

### 吞吐数据

| 并发线程 | EventAsync notify_qps | EventAsync qps/thread | SingleAsync notify_qps | SingleAsync qps/thread |
|---:|---:|---:|---:|---:|
| 1 | 743020 | 743020 | 519516 | 519516 |
| 2 | 602050 | 301025 | 904078 | 452039 |
| 4 | 611652 | 152913 | 724308 | 181077 |
| 8 | 684632 | 85579 | 720147 | 90018.4 |
| 16 | 1130230 | 70639.3 | 999099 | 62443.7 |
| 24 | 1174590 | 48941.2 | 1058450 | 44102.2 |
| 32 | 1147740 | 35866.9 | 1159950 | 36248.4 |

### 原始命令输出摘要

EventAsync 在本轮中所有并发下 `callback_count == notify_success`，但从 2 线程开始 `notify_success < total_notify_attempts`。

SingleAsync 是单回调聚合模型，本轮 callback 数如下：

| 并发线程 | callback_count | coalesced | coalesced_rate_pct |
|---:|---:|---:|---:|
| 1 | 24 | 4072 | 99.4141 |
| 2 | 1 | 2148 | 99.9535 |
| 4 | 2 | 1396 | 99.8569 |
| 8 | 1 | 918 | 99.8912 |
| 16 | 2 | 811 | 99.7540 |
| 24 | 1 | 841 | 99.8812 |
| 32 | 1 | 793 | 99.8741 |
