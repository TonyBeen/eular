/*************************************************************************
    > File Name: log_format.h
    > Author: hsz
    > Mail:
    > Created Time: 2021年04月12日 星期一 22时16分17秒
 ************************************************************************/

#ifndef __LOG_FORMAT_H__
#define __LOG_FORMAT_H__

#include "log_level.h"
#include "log_event.h"
#include <string>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <atomic>

#define PERFIX_SIZE (128)
#define LOG_BUF_SIZE (2048)

// 05-24 10:10:10.100 12345 12345 I TAG: msg(\n)
namespace eular {
class LogFormat {
public:
    LogFormat();
    ~LogFormat();

    LogFormat(const LogFormat&) = delete;
    LogFormat& operator=(const LogFormat&) = delete;

    static std::string Format(const LogEvent *ev);
    static std::string Format(const LogEvent *ev, bool enableColor);
    static const char *LevelColor(LogLevel::Level level);

private:
};

} // namespace eular

#endif // __LOG_FORMAT_H__