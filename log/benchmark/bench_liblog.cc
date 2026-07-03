/*************************************************************************
    > File Name: beach_liblog.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年06月30日 星期日 15时04分44秒
 ************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <string>
#include <random>

#include <log/log.h>

#define LOG_TAG "bench"

std::string generate_random_string(size_t length)
{
    static const std::string visible_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;':,.<>?";
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dis(0, visible_chars.size() - 1);

    std::string random_string;
    random_string.reserve(length);

    for (size_t i = 0; i < length; i++) {
        random_string += visible_chars[dis(gen)];
    }

    return random_string;
}

int main(int argc, char **argv)
{
    int recycle = 50000;
    if (argc > 1) {
        recycle = atoi(argv[1]);
        if (recycle <= 0) {
            recycle = 50000;
        }
    }

    log_set_level(LEVEL_INFO);
    log_set_path("./", "log");
    log_del_output_node(STDOUT);
    log_add_output_node(FILEOUT);

    std::string logMsg;
    logMsg.reserve(256);

    size_t total_ns = 0;
    for (int i = 0; i < recycle; i++)
    {
        logMsg = std::to_string(i) + ": " + generate_random_string(128);
        const auto begin = std::chrono::high_resolution_clock::now();
        LOGW("%s", logMsg.c_str());
        const auto end = std::chrono::high_resolution_clock::now();
        total_ns += static_cast<size_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    }

    printf("Average log call elapsed time: %zuns (recycle=%d)\n", total_ns / static_cast<size_t>(recycle), recycle);
    return 0;
}
