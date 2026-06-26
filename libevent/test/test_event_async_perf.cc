/*************************************************************************
    > File Name: test_event_async_perf.cc
    > Author: Codex
    > Brief: EventAsync 单通知往返延迟基准
 ************************************************************************/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <event/async.h>
#include <event2/event.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    int         threadCount = 1;
    std::size_t iterationsPerThread = 100000;
};

// 每个 worker 拥有一个独立的 EventAsync，但都挂在同一个 event_base 上。
// worker 线程发送一次 notify 后，自旋等待 loop 线程回调确认，然后再进入下一轮。
struct Worker {
    explicit Worker(event_base* loopBase) :
        async(loopBase)
    {
    }

    bool Init()
    {
        if (!async.addAsync(kAsyncId, [this](ev::EventAsync::AsyncId) { OnNotify(); })) {
            return false;
        }

        return async.start();
    }

    void OnNotify()
    {
        acked.fetch_add(1, std::memory_order_release);
    }

    void Run()
    {
        for (std::size_t i = 0; i < iterations; ++i) {
            const std::size_t before = acked.load(std::memory_order_acquire);
            while (!async.notify(kAsyncId)) {
                if (aborted.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            sent.fetch_add(1, std::memory_order_release);

            while (acked.load(std::memory_order_acquire) <= before) {
                if (aborted.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    }

    static constexpr ev::EventAsync::AsyncId kAsyncId = 1;

    ev::EventAsync             async;
    std::atomic<std::size_t>   acked {0};
    std::atomic<std::size_t>   sent {0};
    std::atomic<bool>          aborted {false};
    std::size_t                iterations = 0;
};

Configuration ParseArgs(int argc, char** argv)
{
    Configuration cfg;
    if (argc > 1) {
        cfg.iterationsPerThread = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
        if (cfg.iterationsPerThread == 0) {
            cfg.iterationsPerThread = 100000;
        }
    }
    return cfg;
}

double RunBenchmark(const Configuration& cfg)
{
    event_base* base = event_base_new();
    if (base == nullptr) {
        std::fprintf(stderr, "failed to create event_base\n");
        return -1.0;
    }

    std::vector<std::unique_ptr<Worker> > workers;
    workers.reserve(static_cast<std::size_t>(cfg.threadCount));
    for (int i = 0; i < cfg.threadCount; ++i) {
        std::unique_ptr<Worker> worker(new Worker(base));
        worker->iterations = cfg.iterationsPerThread;
        if (!worker->Init()) {
            std::fprintf(stderr, "worker %d init failed\n", i);
            workers.clear();
            event_base_free(base);
            return -1.0;
        }
        workers.push_back(std::move(worker));
    }

    struct WatchdogContext {
        std::vector<std::unique_ptr<Worker> >* workers = nullptr;
        std::size_t total = 0;
        std::size_t lastAcked = std::numeric_limits<std::size_t>::max();
        int idleTicks = 0;
        bool timedOut = false;
        event_base* base = nullptr;
        event* timer = nullptr;
    };

    WatchdogContext watchdog;
    watchdog.workers = &workers;
    watchdog.total = static_cast<std::size_t>(cfg.threadCount) * cfg.iterationsPerThread;
    watchdog.base = base;

    timeval oneSecond;
    oneSecond.tv_sec = 1;
    oneSecond.tv_usec = 0;
    watchdog.timer = event_new(base, -1, EV_PERSIST, [](evutil_socket_t, short, void* data) {
        WatchdogContext* ctx = static_cast<WatchdogContext*>(data);
        std::size_t totalAcked = 0;
        std::size_t totalSent = 0;
        for (std::size_t i = 0; i < ctx->workers->size(); ++i) {
            totalAcked += (*ctx->workers)[i]->acked.load(std::memory_order_acquire);
            totalSent += (*ctx->workers)[i]->sent.load(std::memory_order_acquire);
        }

        if (totalAcked >= ctx->total) {
            event_base_loopbreak(ctx->base);
            return;
        }

        if (ctx->lastAcked == totalAcked) {
            ++ctx->idleTicks;
        } else {
            ctx->lastAcked = totalAcked;
            ctx->idleTicks = 0;
        }

        // 异常保护: 正常基准不会走到这里。10秒无任何回调进展时中止, 避免测试永久卡死。
        if (ctx->idleTicks >= 10) {
            ctx->timedOut = true;
            std::fprintf(stderr,
                         "benchmark stalled: sent=%zu acked=%zu expected=%zu\n",
                         totalSent,
                         totalAcked,
                         ctx->total);
            for (std::size_t i = 0; i < ctx->workers->size(); ++i) {
                (*ctx->workers)[i]->aborted.store(true, std::memory_order_release);
            }
            event_base_loopbreak(ctx->base);
        }
    }, &watchdog);
    if (watchdog.timer == nullptr || event_add(watchdog.timer, &oneSecond) != 0) {
        std::fprintf(stderr, "failed to create watchdog timer\n");
        if (watchdog.timer != nullptr) {
            event_free(watchdog.timer);
        }
        workers.clear();
        event_base_free(base);
        return -1.0;
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.threadCount));

    const auto start = Clock::now();
    for (std::size_t i = 0; i < workers.size(); ++i) {
        Worker* worker = workers[i].get();
        threads.emplace_back([worker] { worker->Run(); });
    }

    const std::size_t total = static_cast<std::size_t>(cfg.threadCount) * cfg.iterationsPerThread;
    std::size_t totalAcked = 0;
    while (totalAcked < total) {
        const int rc = event_base_loop(base, EVLOOP_ONCE);
        if (rc < 0 || watchdog.timedOut) {
            break;
        }
        totalAcked = 0;
        for (std::size_t i = 0; i < workers.size(); ++i) {
            totalAcked += workers[i]->acked.load(std::memory_order_acquire);
        }
    }

    for (std::size_t i = 0; i < threads.size(); ++i) {
        if (threads[i].joinable()) {
            threads[i].join();
        }
    }

    event_del(watchdog.timer);
    event_free(watchdog.timer);

    if (watchdog.timedOut || totalAcked < total) {
        // 先销毁 EventAsync, 再释放 event_base; 反过来会触发 event_del/event_free 的 use-after-free。
        workers.clear();
        event_base_free(base);
        return -1.0;
    }

    const auto end = Clock::now();
    const std::chrono::duration<double, std::nano> elapsed = end - start;

    const std::size_t totalNotifies =
        static_cast<std::size_t>(cfg.threadCount) * cfg.iterationsPerThread;
    const double avgNs = elapsed.count() / static_cast<double>(totalNotifies);
    const double totalSeconds = elapsed.count() / 1e9;
    const double tps = static_cast<double>(totalNotifies) / totalSeconds;

    std::printf("| %6d | %14zu | %14.3f | %16.3f | %14.3f |\n",
                cfg.threadCount,
                cfg.iterationsPerThread,
                totalSeconds * 1000.0,
                avgNs / 1000.0,
                tps / 1000.0);

    // 先销毁 EventAsync, 再释放 event_base; 反过来会触发 event_del/event_free 的 use-after-free。
    workers.clear();
    event_base_free(base);
    return avgNs;
}

} // namespace

int main(int argc, char** argv)
{
    const Configuration args = ParseArgs(argc, argv);

    std::printf("EventAsync single-notification latency benchmark\n");
    std::printf("Compiler: %s\n",
#if defined(__VERSION__)
                __VERSION__
#else
                "unknown"
#endif
                );
    std::printf("Iterations per thread: %zu\n\n", args.iterationsPerThread);

    std::printf("+--------+----------------+----------------+------------------+----------------+\n");
    std::printf("| %6s | %14s | %14s | %16s | %14s |\n",
                "threads", "iters/thread", "total(ms)", "avg(us)/notify", "k_notifies/s");
    std::printf("+--------+----------------+----------------+------------------+----------------+\n");

    const int threadCounts[] = {1, 2, 4, 8, 16, 24, 32};
    double bestAvg = 1e18;
    int bestThreads = 0;

    for (std::size_t i = 0; i < sizeof(threadCounts) / sizeof(threadCounts[0]); ++i) {
        Configuration cfg;
        cfg.threadCount = threadCounts[i];
        cfg.iterationsPerThread = args.iterationsPerThread;
        const double avg = RunBenchmark(cfg);
        if (avg > 0.0 && avg < bestAvg) {
            bestAvg = avg;
            bestThreads = cfg.threadCount;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("+--------+----------------+----------------+------------------+----------------+\n");
    if (bestThreads > 0) {
        std::printf("\nBest avg latency: %.3f us/notify @ %d threads\n",
                    bestAvg / 1000.0,
                    bestThreads);
    }
    return 0;
}
