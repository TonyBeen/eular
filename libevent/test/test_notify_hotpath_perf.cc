/*************************************************************************
    > File Name: test_notify_hotpath_perf.cc
    > Author: Codex
    > Brief: 对比 EventAsync 和 SingleAsync 的 notify 热路径性能
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

#include <event/async.h>
#include <event/loop.h>
#include <event/single_async.h>
#include <event2/event.h>

namespace {
struct Options {
    uint32_t producerThreads = 4;
    uint32_t rounds = 200;
    uint32_t notifyPerRound = 512;
    uint32_t idPoolSize = 64;
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
        if (std::strcmp(arg, "--producers") == 0 && i + 1 < argc) {
            opt.producerThreads = static_cast<uint32_t>(ParseU64(argv[++i], opt.producerThreads));
        } else if (std::strcmp(arg, "--rounds") == 0 && i + 1 < argc) {
            opt.rounds = static_cast<uint32_t>(ParseU64(argv[++i], opt.rounds));
        } else if (std::strcmp(arg, "--per-round") == 0 && i + 1 < argc) {
            opt.notifyPerRound = static_cast<uint32_t>(ParseU64(argv[++i], opt.notifyPerRound));
        } else if (std::strcmp(arg, "--ids") == 0 && i + 1 < argc) {
            opt.idPoolSize = static_cast<uint32_t>(ParseU64(argv[++i], opt.idPoolSize));
        }
    }

    if (opt.producerThreads == 0) {
        opt.producerThreads = 1;
    }
    if (opt.rounds == 0) {
        opt.rounds = 1;
    }
    if (opt.notifyPerRound == 0) {
        opt.notifyPerRound = 1;
    }
    if (opt.idPoolSize == 0) {
        opt.idPoolSize = 1;
    }

    return opt;
}

struct Result {
    uint64_t attempts = 0;
    uint64_t success = 0;
    double elapsedSec = 0.0;
};

template <typename NotifyFn>
Result RunOneRound(uint32_t producerThreads, uint32_t notifyPerRound, NotifyFn notifyFn)
{
    Result result;
    result.attempts = notifyPerRound;

    std::atomic<uint64_t> success(0);
    std::atomic<uint32_t> readyThreads(0);
    std::atomic<bool> startFlag(false);
    const uint64_t perThread = notifyPerRound / producerThreads;
    const uint64_t remainder = notifyPerRound % producerThreads;

    std::vector<std::thread> producers;
    producers.reserve(producerThreads);
    for (uint32_t t = 0; t < producerThreads; ++t) {
        producers.emplace_back([&, t]() {
            readyThreads.fetch_add(1, std::memory_order_release);
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const uint64_t localCount = perThread + (t < remainder ? 1 : 0);
            for (uint64_t i = 0; i < localCount; ++i) {
                if (notifyFn(t, i)) {
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (readyThreads.load(std::memory_order_acquire) != producerThreads) {
        std::this_thread::yield();
    }

    const auto t0 = std::chrono::steady_clock::now();
    startFlag.store(true, std::memory_order_release);
    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }
    const auto t1 = std::chrono::steady_clock::now();

    result.success = success.load(std::memory_order_relaxed);
    result.elapsedSec = std::chrono::duration_cast<std::chrono::duration<double> >(t1 - t0).count();
    return result;
}

Result BenchmarkEventAsync(const Options& opt)
{
    Result result;
    result.attempts = static_cast<uint64_t>(opt.rounds) * static_cast<uint64_t>(opt.notifyPerRound);

    for (uint32_t round = 0; round < opt.rounds; ++round) {
        ev::EventLoop::SP loop = std::make_shared<ev::EventLoop>();
        event_base* base = loop->loop();
        ev::EventAsync async(base);
        for (uint32_t i = 1; i <= opt.idPoolSize; ++i) {
            async.addAsync(i, [](ev::EventAsync::AsyncId) {});
        }
        async.start();
        std::atomic<bool> stopDrain(false);
        std::thread drainThread([base, &stopDrain]() {
            while (!stopDrain.load(std::memory_order_acquire)) {
                event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
                usleep(50);
            }
        });
        Result one = RunOneRound(opt.producerThreads, opt.notifyPerRound, [&](uint32_t threadIndex, uint64_t seq) {
            const ev::EventAsync::AsyncId id =
                static_cast<ev::EventAsync::AsyncId>((seq + threadIndex) % opt.idPoolSize + 1);
            return async.notify(id);
        });
        stopDrain.store(true, std::memory_order_release);
        drainThread.join();
        result.success += one.success;
        result.elapsedSec += one.elapsedSec;
    }

    return result;
}

Result BenchmarkSingleAsync(const Options& opt)
{
    Result result;
    result.attempts = static_cast<uint64_t>(opt.rounds) * static_cast<uint64_t>(opt.notifyPerRound);

    for (uint32_t round = 0; round < opt.rounds; ++round) {
        ev::EventLoop::SP loop = std::make_shared<ev::EventLoop>();
        event_base* base = loop->loop();
        ev::SingleAsync async(base, []() {});
        async.start();
        std::atomic<bool> stopDrain(false);
        std::thread drainThread([base, &stopDrain]() {
            while (!stopDrain.load(std::memory_order_acquire)) {
                event_base_loop(base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
                usleep(50);
            }
        });
        Result one = RunOneRound(opt.producerThreads, opt.notifyPerRound, [&](uint32_t, uint64_t) {
            return async.notify();
        });
        stopDrain.store(true, std::memory_order_release);
        drainThread.join();
        result.success += one.success;
        result.elapsedSec += one.elapsedSec;
    }

    return result;
}
} // namespace

int main(int argc, char** argv)
{
    const Options opt = ParseArgs(argc, argv);

    const Result eventAsync = BenchmarkEventAsync(opt);
    const Result singleAsync = BenchmarkSingleAsync(opt);

    const double eventAvgNs = eventAsync.success > 0
        ? (eventAsync.elapsedSec * 1000000000.0) / static_cast<double>(eventAsync.success)
        : 0.0;
    const double singleAvgNs = singleAsync.success > 0
        ? (singleAsync.elapsedSec * 1000000000.0) / static_cast<double>(singleAsync.success)
        : 0.0;

    std::cout << "==== Notify Hotpath Perf ====" << std::endl;
    std::cout << "producer_threads=" << opt.producerThreads << std::endl;
    std::cout << "rounds=" << opt.rounds << std::endl;
    std::cout << "notify_per_round=" << opt.notifyPerRound << std::endl;
    std::cout << "event_async_success=" << eventAsync.success << "/" << eventAsync.attempts << std::endl;
    std::cout << "event_async_avg_notify_ns=" << eventAvgNs << std::endl;
    std::cout << "single_async_success=" << singleAsync.success << "/" << singleAsync.attempts << std::endl;
    std::cout << "single_async_avg_notify_ns=" << singleAvgNs << std::endl;
    std::cout << "single_vs_event_ratio="
              << (eventAvgNs > 0.0 ? singleAvgNs / eventAvgNs : 0.0)
              << std::endl;

    return 0;
}
