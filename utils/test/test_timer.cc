#include <signal.h>

#include <utils/timer.h>
#include <utils/platform.h>

using namespace eular;

TimerManager gTimerManager;

void func(void *arg)
{
    printf("func()\n");
}

void func2(void *arg)
{
    printf("func2()\n");
}

void test_main_loop()
{
    std::shared_ptr<void> ptr((new int(10)));
    gTimerManager.addTimer(2000, std::bind(func2, ptr.get()), 1000);
    gTimerManager.addTimer(6000, std::bind(func, nullptr), 2000);
    gTimerManager.startTimer(true);
}

void test_thread_loop()
{
    gTimerManager.startTimer();
    gTimerManager.addTimer(2000, std::bind(func2, nullptr), 1000);
    gTimerManager.addTimer(6000, std::bind(func, nullptr), 2000);

    eular_sleep(2);
    gTimerManager.stopTimer();
}

void catchSignal(int sig)
{
    exit(0);
}

int main()
{
    signal(SIGSEGV, catchSignal);
    test_thread_loop();

    eular_sleep(1);
    return 0;
}
