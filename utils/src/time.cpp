/*************************************************************************
    > File Name: time.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月18日 星期五 15时47分18秒
 ************************************************************************/

#include "utils/time.h"
#include "utils/sysdef.h"

#include <ctime>
#include <string.h>
#include <chrono>

namespace eular {
uint64_t Time::SystemTime()
{
    std::chrono::system_clock::time_point tm = std::chrono::system_clock::now();
    std::chrono::milliseconds mills = 
        std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
    return mills.count();
}

uint64_t Time::AbsTime()
{
    std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
    std::chrono::milliseconds mills = 
        std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
    return mills.count();
}

std::string Time::GmtTime(const time_t *tim)
{
    time_t t = std::time(nullptr);
    if (tim != nullptr) {
        t = *tim;
    }

    tm stm;
    memset(&stm, 0, sizeof(stm));
#if defined(OS_LINUX) || defined(OS_APPLE)
    gmtime_r(&t, &stm);
#else
    gmtime_s(&stm, &t);
#endif

    char buf[256] = {0};
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &stm);
    return std::string(buf);
}

std::string Time::Format(time_t time, const std::string& format)
{
    return Format(time, format.c_str());
}

std::string Time::Format(time_t time, const char *format)
{
    char buf[256] = {0};
    tm stm;
    memset(&stm, 0, sizeof(stm));
#if defined(OS_LINUX) || defined(OS_APPLE)
    localtime_r(&time, &stm);
#else
    localtime_s(&stm, &time);
#endif
    strftime(buf, sizeof(buf), format, &stm);
    return std::string(buf);
}

time_t Time::Parse(const std::string &timeStr, const std::string &format)
{
    return Parse(timeStr.c_str(), format.c_str());
}

time_t Time::Parse(const char *timeStr, const char *format)
{
    struct tm stm;
    memset(&stm, 0, sizeof(stm));
#if defined(OS_LINUX) || defined(OS_APPLE)
    if (strptime(timeStr, format, &stm) == nullptr) {
        return -1;
    }
#else
    if (strptime_s(timeStr, strlen(timeStr), format, &stm) != 0) {
        return -1;
    }
#endif
    stm.tm_isdst = -1; // 让系统自动判断夏令时
    return mktime(&stm);
}
} // namespace eular
