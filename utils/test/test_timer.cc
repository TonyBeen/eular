#include <unistd.h>
#include <signal.h>

#include <utils/timer.h>
#include <log/log.h>
#include <log/callstack.h>

#define LOG_TAG "test_timer"

using namespace eular;

TimerManager gTimerManager;

void func(void *arg)
{
    LOGI("func()");
}

void func2(void *arg)
{
    LOGI("func2()");
}

void test_main_loop()
{
    std::shared_ptr<void> ptr((new int(10)));
    gTimerManager.addTimer(2000, std::bind(func2, ptr.get()), 1000);
    uint64_t uniqueId = gTimerManager.addTimer(6000, std::bind(func, nullptr), 2000);
    LOGI("timer start");
    gTimerManager.startTimer(true);
}

void test_thread_loop()
{
    LOGI("timer start");
    gTimerManager.startTimer();
    gTimerManager.addTimer(2000, std::bind(func2, nullptr), 1000);
    uint64_t uniqueId = gTimerManager.addTimer(6000, std::bind(func, nullptr), 2000);

    sleep(2);
    gTimerManager.stopTimer();
}

void catchSignal(int sig)
{
    if (sig == SIGSEGV) {
        CallStack stack;
        stack.update();
        stack.log(LOG_TAG, LogLevel::LEVEL_FATAL);
    }
    exit(0);
}

int main()
{
    signal(SIGSEGV, catchSignal);
    test_thread_loop();

    sleep(1);
    return 0;
}
