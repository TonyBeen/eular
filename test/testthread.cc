/*************************************************************************
    > File Name: testthread.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年06月19日 星期六 17时53分21秒
 ************************************************************************/

#include <stdio.h>
#include <utils/thread.h>
#include <utils/string8.h>

struct Data {
    int num;
    Alias::String8 str;
};

int function(void *arg)
{
    printf("--- %s: %s()\n", __FILE__, __func__);
    if (arg) {
        Data *data = static_cast<Data *>(arg);
        printf("data[%d, %s]\n", data->num, data->str.c_str());
    }
    int ms = 10;
    printf("--- sleep %d ms\n", ms);
    msleep(ms);
    return -1;
}

int main(int argc, char **argv)
{
    Alias::Thread thread("test", function);
    Data *data = new Data;
    if (data == nullptr) {
        perror("malloc");
        return -1;
    }
    data->num = 100;
    data->str = "Hello world!";
    printf("%s: %s() thread run\n", __FILE__, __func__);
    thread.setArg(data);
    thread.run();
    printf("%s: %s() thread status %d\n", __FILE__, __func__, thread.ThreadStatus());
    sleep(2);
    printf("func return %d\n", thread.getFunctionReturn());
    printf("func name -> %s\n", thread.GetThreadName().c_str());

    if (thread.ThreadStatus() == Alias::ThreadBase::THREAD_WAITING) {
        thread.StartWork();
    }

    usleep(1);
    thread.Interrupt();
    sleep(1);
    return 0;
}