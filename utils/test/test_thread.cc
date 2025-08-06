/*************************************************************************
    > File Name: testthread.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年06月19日 星期六 17时53分21秒
 ************************************************************************/

#include <stdio.h>
#include <utils/platform.h>
#include <utils/thread.h>
#include <utils/string8.h>

using namespace eular;

struct Data {
    int num;
    eular::String8 str;
};

class ThreadExt : public ThreadBase {
public:
    ThreadExt() : ThreadBase("test-drived-thread") {}

    void join()
    {
        int ret = pthread_join(mTid, nullptr);
        if (ret) {
            perror("pthread_join error");
        }
    }

protected:
    virtual int threadWorkFunction(void *arg) override
    {
        printf("ThreadExt::threadWorkFunction() and thread will wait...\n");
        return THREAD_WAITING;
    }
};

int function(void *arg)
{
    printf("--- %s: %s()\n", __FILE__, __func__);
    if (arg) {
        Data *data = static_cast<Data *>(arg);
        printf("data[%d, %s]\n", data->num, data->str.c_str());
    }
    int ms = 10;
    printf("--- sleep %d ms tid = %d\n", ms, gettid());
    eular_msleep(ms);
    return -1;
}

int main(int argc, char **argv)
{
    Data *data = new Data;
    if (data == nullptr) {
        perror("malloc");
        return -1;
    }
    data->num = 100;
    data->str = "Hello world!";

    ThreadExt ext;
    ext.start();
    printf("ext tid %d\n", ext.getKernalTid());
    printf("thread name: %s\n", ext.threadName().c_str());
    usleep(500 * 1000);

    printf("restart thread...\n");
    ext.start();
    usleep(500 * 1000);

    ext.stop();
    delete data;
    ext.join();
    printf("\033[32mWhen you see this sentence, the program runs successfully\033[0m\n");
    return 0;
}
