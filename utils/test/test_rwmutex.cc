#include <utils/platform.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "catch/catch.hpp"

#include <utils/mutex.h>

using namespace std;
using namespace eular;

namespace {

bool wait_for_true(const std::atomic<bool> &value, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (value.load()) {
            return true;
        }
        eular_msleep(5);
    }
    return value.load();
}

} // namespace

TEST_CASE("rwmutex_writer_waits_for_reader", "[rwmutex]")
{
    RWMutex mutex;
    std::atomic<bool> readerHolding(false);
    std::atomic<bool> writerAcquired(false);

    std::thread reader([&]() {
        RDAutoLock<RWMutex> lock(mutex);
        readerHolding = true;
        eular_msleep(80);
    });

    REQUIRE(wait_for_true(readerHolding, 200));

    std::thread writer([&]() {
        WRAutoLock<RWMutex> lock(mutex);
        writerAcquired = true;
    });

    eular_msleep(20);
    CHECK_FALSE(writerAcquired.load());

    reader.join();
    REQUIRE(wait_for_true(writerAcquired, 200));
    writer.join();
}

TEST_CASE("rwmutex_allows_multiple_readers", "[rwmutex]")
{
    RWMutex mutex;
    std::atomic<int> concurrentReaders(0);
    std::atomic<int> maxConcurrentReaders(0);

    auto reader = [&]() {
        RDAutoLock<RWMutex> lock(mutex);
        const int current = ++concurrentReaders;
        maxConcurrentReaders = std::max(maxConcurrentReaders.load(), current);
        eular_msleep(40);
        --concurrentReaders;
    };

    std::thread t1(reader);
    std::thread t2(reader);
    t1.join();
    t2.join();

    CHECK(maxConcurrentReaders.load() >= 2);
}