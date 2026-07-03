#include "log.h"
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "Test"

#define CYCLE_TIMES 5

void *thread(void *)
{
    int num = 0;
    while (num < CYCLE_TIMES) {
        LOGI("thread-%.5d", num++);
        usleep(500000);
    }

    return nullptr;
}

int main()
{
    log_set_level(LEVEL_DEBUG);
    log_set_path("./", "log");
    log_enable_color(1);
    log_add_output_node(FILEOUT);

    LOGD("**************");
    LOGI("**************");
    LOGW("**************");
    LOGE("**************");
    LOGF("**************");

    pthread_t tid;
    pthread_create(&tid, nullptr, thread, nullptr);
    int num = 0;
    while (num < CYCLE_TIMES) {
        LOGI("main-%d", num++);
        usleep(500000);
    }

    pthread_join(tid, nullptr);
    return 0;
}