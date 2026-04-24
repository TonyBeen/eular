/*************************************************************************
    > File Name: beach_liblog.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年06月30日 星期日 15时04分44秒
 ************************************************************************/

#include <stdint.h>
#include <string>
#include <random>

#include <utils/elapsed_time.h>
#include <log/log.h>

#define LOG_TAG "bench"

std::string generate_random_string(int length)
{
    std::string visible_chars = 
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;':,.<>?";

    std::string random_string;
    random_string.reserve(length);

    // 生成指定长度随机字符串
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, visible_chars.length() - 1);

    for (int i = 0; i < length; i++) {
        random_string += visible_chars[dis(gen)];
    }

    return random_string;
}

int main()
{
    eular::log::InitLog(eular::LogLevel::LEVEL_INFO);
    eular::log::SetPath("./");
    eular::log::delOutputNode(eular::LogWrite::STDOUT);
    eular::log::addOutputNode(eular::LogWrite::FILEOUT);

    std::string logMsg(512, '\0');

    int recycle = 50000;
    eular::ElapsedTime et(ElapsedTimeType::NANOSECOND);
    for (int i = 0; i < recycle; i++)
    {
        logMsg = std::to_string(i) + ": " + generate_random_string(128);
        et.start();
        LOGW("%s", logMsg.c_str());
        et.stop();
    }

    printf("Elapsed time: %luns\n", et.elapsedTime() / recycle);
    return 0;
}
