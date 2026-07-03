/*************************************************************************
    > File Name: log_level.h
    > Author: hsz
    > Desc: This file is for log level.
    > Created Time: 2021年04月12日 星期一 22时16分04秒
 ************************************************************************/

#ifndef __LOG_LOG_LEVEL_H__
#define __LOG_LOG_LEVEL_H__

#include <log/log.h>

#include <string>
#include <string.h>

namespace eular {

enum class OutputType {
    STDOUT = 0,
    FILEOUT = 1,
    CONSOLEOUT = 2,
    UNKNOW
};

class LogLevel {
public:
    using Level = log_level_t;
    enum {
        UNKNOW = LEVEL_UNKNOWN,
        LEVEL_DEBUG = ::LEVEL_DEBUG,
        LEVEL_INFO = ::LEVEL_INFO,
        LEVEL_WARN = ::LEVEL_WARN,
        LEVEL_ERROR = ::LEVEL_ERROR,
        LEVEL_FATAL = ::LEVEL_FATAL,
    };

    static const char *ToString(Level level)
    {
        switch (level) {
        case LEVEL_DEBUG:
            return "DEBUG";
        case LEVEL_INFO:
            return "INFO";
        case LEVEL_WARN:
            return "WARN";
        case LEVEL_ERROR:
            return "ERROR";
        case LEVEL_FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
        }
    }

    static const char *ToFormatString(Level level)
    {
        switch (level) {
        case LEVEL_DEBUG:
            return "[D]";
        case LEVEL_INFO:
            return "[I]";
        case LEVEL_WARN:
            return "[W]";
        case LEVEL_ERROR:
            return "[E]";
        case LEVEL_FATAL:
            return "[F]";
        default:
            return "[?]";
        }
    }

    static Level String2Level(const std::string& lev)
    {
        if (::strcasecmp(lev.c_str(), "debug") == 0) {
            return static_cast<Level>(LEVEL_DEBUG);
        }
        if (::strcasecmp(lev.c_str(), "info") == 0) {
            return static_cast<Level>(LEVEL_INFO);
        }
        if (::strcasecmp(lev.c_str(), "warn") == 0) {
            return static_cast<Level>(LEVEL_WARN);
        }
        if (::strcasecmp(lev.c_str(), "error") == 0) {
            return static_cast<Level>(LEVEL_ERROR);
        }
        if (::strcasecmp(lev.c_str(), "fatal") == 0) {
            return static_cast<Level>(LEVEL_FATAL);
        }
        return static_cast<Level>(LEVEL_UNKNOWN);
    }
};
} // namespace eular
#endif // #ifndef __LOG_LEVEL_H__