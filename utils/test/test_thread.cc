#include <atomic>

#include "catch/catch.hpp"
#include "utils/platform.h"
#include "utils/thread.h"

using namespace eular;

namespace {

bool wait_for_at_least(const std::atomic<int> &value, int expected, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (value.load() >= expected) {
            return true;
        }
        eular_msleep(5);
    }
    return value.load() >= expected;
}

class CountingThread : public ThreadBase {
public:
    CountingThread() : ThreadBase("counting"), runs(0) {}

    std::atomic<int> runs;

protected:
    int threadWorkFunction(void *) override
    {
        const int current = ++runs;
        return current >= 2 ? THREAD_EXIT : THREAD_WAITING;
    }
};

} // namespace

TEST_CASE("thread_executes_callback_and_exposes_thread_local_state", "[thread]") {
    std::atomic<bool> ran(false);
    std::atomic<bool> sawThis(false);
    String8 observedName;
    int32_t observedTid = 0;

    Thread worker([&]() {
        sawThis = (Thread::GetThis() != nullptr);
        Thread::SetName("renamed");
        observedName = Thread::GetName();
        observedTid = gettid();
        ran = true;
    }, "worker");

    worker.join();

    CHECK(ran.load());
    CHECK(sawThis.load());
    CHECK(observedName.toStdString() == "renamed");
    CHECK(worker.getName().toStdString() == "renamed");
    CHECK(worker.getTid() == observedTid);
}

TEST_CASE("thread_base_can_be_restarted_from_waiting_state", "[thread]") {
    CountingThread worker;

    CHECK(worker.start() == 0);
    REQUIRE(wait_for_at_least(worker.runs, 1, 1000));
    CHECK(worker.getTid() != 0);

    CHECK(worker.start() == 0);
    REQUIRE(wait_for_at_least(worker.runs, 2, 1000));

    worker.join();
    CHECK(worker.threadStatus() == static_cast<uint32_t>(ThreadBase::THREAD_EXIT));
}