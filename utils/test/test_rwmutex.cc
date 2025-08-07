#include <utils/mutex.h>
#include <utils/utils.h>
#include <utils/string8.h>
#include <utils/thread.h>
#include <utils/platform.h>

#include <assert.h>
#include <stdio.h>
#include <atomic>

using namespace std;
using namespace eular;

static char buf[128] = {0};
RWMutex gRwMutex;
std::atomic<bool> gExit;

int read_func(void *)
{
    while (gExit == false) {
        {
            RDAutoLock<RWMutex> lock(gRwMutex);
            printf("[%d]buf: %s\n", gettid(), buf);
        }
        eular_usleep(100 * 1000);
    }

    return 0;
}

int write_func(void *)
{
    int num = 0;
    while (gExit == false) {
        {
            WRAutoLock<RWMutex> lock(gRwMutex);
            snprintf(buf, 127, "num = %d", ++num);
        }
        eular_usleep(50 * 1000);
    }

    return 0;
}

int main()
{
    gExit = false;
    Thread t1(std::bind(read_func, nullptr), "read");
    Thread t2(std::bind(write_func, nullptr), "write");

    sleep(2);
    gExit = true;

    t1.join();
    t2.join();
    return 0;
}