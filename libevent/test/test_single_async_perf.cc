/*************************************************************************
    > File Name: test_single_async_perf.cc
    > Author: Codex
    > Brief: SingleAsync 单通知性能测试
 ************************************************************************/

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>

#include <event2/event.h>
#include <event/loop.h>
#include <event/single_async.h>

namespace {
struct Options {
    uint64_t totalNotify = 1000000;
    uint32_t producerThreads = 1;
    uint32_t drainMs = 1000;
    uint32_t warmupMs = 200;
};

uint64_t ParseU64(const char* s, uint64_t defaultValue)
{
    if (s == nullptr || *s == '\0') {
        return defaultValue;
    }

    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return defaultValue;
    }
    return static_cast<uint64_t>(v);
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--total") == 0 && i + 1 < argc) {
            opt.totalNotify = ParseU64(argv[++i], opt.totalNotify);
        } else if (std::strcmp(arg, "--producers") == 0 && i + 1 < argc) {
            opt.producerThreads = static_cast<uint32_t>(ParseU64(argv[++i], opt.producerThreads));
        } else if (std::strcmp(arg, "--drain-ms") == 0 && i + 1 < argc) {
            opt.drainMs = static_cast<uint32_t>(ParseU64(argv[++i], opt.drainMs));
        } else if (std::strcmp(arg, "--warmup-ms") == 0 && i + 1 < argc) {
            opt.warmupMs = static_cast<uint32_t>(ParseU64(argv[++i], opt.warmupMs));
        } else if (std::strcmp(arg, "--help") == 0) {
            std::cout
                << "Usage: test_single_async_perf [--total N] [--producers N] "
                   "[--drain-ms N] [--warmup-ms N]\n"
                << "  --total      total notify attempts, default 1000000\n"
                << "  --producers  producer thread count, default 1\n"
                << "  --drain-ms   wait time after producers finish, only for callback drain stats\n"
                << "  --warmup-ms  wait after all producer threads are ready, default 200\n";
            std::exit(0);
        }
    }

    if (opt.producerThreads == 0) {
        opt.producerThreads = 1;
    }

    return opt;
}
} // namespace

int main(int argc, char** argv)
{
    const Options opt = ParseArgs(argc, argv);

    ev::EventLoop::SP loop = std::make_shared<ev::EventLoop>();
    event_base* base = loop->loop();
    std::atomic<uint64_t> callbackCount(0);
    ev::SingleAsync::SP async = std::make_shared<ev::SingleAsync>(base, [&callbackCount]() {
        callbackCount.fetch_add(1, std::memory_order_relaxed);
    });

    if (!async->start()) {
        std::cerr << "start single async failed\n";
        return 2;
    }

    std::atomic<uint32_t> readyThreads(0);
    std::atomic<bool> startFlag(false);
    std::atomic<uint64_t> notifySuccess(0);
    std::atomic<bool> loopStop(false);

    // loop 线程只负责消费通知, 不参与 notify 热路径计时。
    // 使用 NONBLOCK 保证测试结束时不会卡在 event_base_loop。
    std::thread loopThread([base, &loopStop]() {
        while (!loopStop.load(std::memory_order_acquire)) {
            event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
            usleep(50);
        }
    });

    const uint64_t perThread = opt.totalNotify / opt.producerThreads;
    const uint64_t remainder = opt.totalNotify % opt.producerThreads;

    std::vector<std::thread> producers;
    producers.reserve(opt.producerThreads);
    for (uint32_t t = 0; t < opt.producerThreads; ++t) {
        producers.emplace_back([&, t]() {
            readyThreads.fetch_add(1, std::memory_order_release);
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const uint64_t localCount = perThread + (t < remainder ? 1 : 0);
            for (uint64_t i = 0; i < localCount; ++i) {
                if (async->notify()) {
                    notifySuccess.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (readyThreads.load(std::memory_order_acquire) != opt.producerThreads) {
        std::this_thread::yield();
    }

    if (opt.warmupMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.warmupMs));
    }

    // 计时区间只覆盖生产线程执行 notify() 的时间。
    // callback 可能被聚合或延后消费, 不影响 avg_notify_ns。
    const auto t0 = std::chrono::steady_clock::now();
    startFlag.store(true, std::memory_order_release);

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    const auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.drainMs));
    const auto t2 = std::chrono::steady_clock::now();

    async->stop();
    loopStop.store(true, std::memory_order_release);
    loopThread.join();
    async.reset();
    loop.reset();

    const uint64_t ok = notifySuccess.load(std::memory_order_relaxed);
    const uint64_t cb = callbackCount.load(std::memory_order_relaxed);
    const uint64_t coalesced = (ok >= cb) ? (ok - cb) : 0;
    const double coalescedRate = (ok == 0) ? 0.0 :
        (100.0 * static_cast<double>(coalesced) / static_cast<double>(ok));
    const double sendSec = std::chrono::duration_cast<std::chrono::duration<double> >(t1 - t0).count();
    const double totalSec = std::chrono::duration_cast<std::chrono::duration<double> >(t2 - t0).count();

    std::cout << "==== SingleAsync Notify Hotpath Perf ====" << std::endl;
    std::cout << "total_notify_attempts=" << opt.totalNotify << std::endl;
    std::cout << "producer_threads=" << opt.producerThreads << std::endl;
    std::cout << "drain_ms=" << opt.drainMs << std::endl;
    std::cout << "warmup_ms=" << opt.warmupMs << std::endl;
    std::cout << "notify_success=" << ok << std::endl;
    std::cout << "callback_count=" << cb << std::endl;
    std::cout << "coalesced=" << coalesced << std::endl;
    std::cout << "coalesced_rate_pct=" << coalescedRate << std::endl;
    std::cout << "send_elapsed_sec=" << sendSec << std::endl;
    std::cout << "total_elapsed_sec=" << totalSec << std::endl;
    std::cout << "notify_qps=" << (sendSec > 0.0 ? static_cast<double>(ok) / sendSec : 0.0) << std::endl;
    std::cout << "notify_qps_per_thread="
              << (sendSec > 0.0 && opt.producerThreads > 0
                  ? (static_cast<double>(ok) / sendSec) / static_cast<double>(opt.producerThreads)
                  : 0.0)
              << std::endl;
    std::cout << "avg_notify_ns="
              << (ok > 0 ? (sendSec * 1000000000.0) / static_cast<double>(ok) : 0.0)
              << std::endl;
    std::cout << "callback_qps=" << (totalSec > 0.0 ? static_cast<double>(cb) / totalSec : 0.0) << std::endl;

    return 0;
}
