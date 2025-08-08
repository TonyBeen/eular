/*************************************************************************
    > File Name: test_mutex_2.cc
    > Author: hsz
    > Brief:
    > Created Time: Wed 23 Nov 2022 10:49:35 AM CST
 ************************************************************************/

/**
 * https://blog.csdn.net/weixin_32147807/article/details/116701847
 * 测试当某一线程在未解锁情况下异常退出时处理
 */

#include <assert.h>
#include <utils/thread.h>
#include <utils/mutex.h>
#include <utils/platform.h>

eular::Mutex gMutex;
int count = 0;

eular::once_flag g_runOnce;

void fnPrint()
{
    static uint32_t run_once = 0;
    ++run_once;
    assert(run_once == 1);
}

void thread_1()
{
    uint32_t circle = 0;
    while (circle++ < 10) {
        eular::call_once(g_runOnce, fnPrint);
        gMutex.lock();
        ++count;
        printf("tid = %d, ++count = %d\n", gettid(), count);
        gMutex.unlock();
        eular_msleep(500);
    }
}

void thread_2()
{
    gMutex.lock();
    printf("%s() thread %d exit without unlocking\n", __func__, gettid());
}

void thread_3()
{
    uint32_t circle = 0;
    while (circle++ < 10) {
        gMutex.lock();
        if (count > 0) {
            --count;
        }
        printf("tid = %d, --count = %d\n", gettid(), count);
        gMutex.unlock();
        eular_msleep(400);
    }
}

int main(int argc, char **argv)
{
    eular::Thread th1(std::bind(thread_1));
    eular::Thread th2(std::bind(thread_2));
    th2.join();
    eular_sleep(2);
    eular::Thread th3(std::bind(thread_3));
    th1.join();
    th3.join();

    return 0;
}