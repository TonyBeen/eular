/*************************************************************************
    > File Name: log_level.h
    > Author: hsz
    > Desc: This file is for log level.
    > Created Time: 2021年04月12日 星期一 22时16分04秒
 ************************************************************************/

#ifndef __LOG_LEVEL_H__
#define __LOG_LEVEL_H__

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
    enum Level {
        UNKNOW = -1,
        LEVEL_DEBUG = 0,
        LEVEL_INFO  = 1,
        LEVEL_WARN  = 2,
        LEVEL_ERROR = 3,
        LEVEL_FATAL = 4
    };

    static std::string ToString(Level l)
    {
        std::string str;
        switch (l) {
        case LEVEL_DEBUG:
            str = "DEBUG";
            break;
        case LEVEL_INFO:
            str = "INFO";
            break;
        case LEVEL_WARN:
            str = "WARN";
            break;
        case LEVEL_ERROR:
            str = "ERROR";
            break;
        case LEVEL_FATAL:
            str = "FATAL";
            break;

        default:
            break;
        }
        return str;
    }

    static std::string ToFormatString(Level level)
    {
        std::string str;
        switch (level) {
        case LEVEL_DEBUG:
            str = "[D]";
            break;
        case LEVEL_INFO:
            str = "[I]";
            break;
        case LEVEL_WARN:
            str = "[W]";
            break;
        case LEVEL_ERROR:
            str = "[E]";
            break;
        case LEVEL_FATAL:
            str = "[F]";
            break;
        
        default:
            str = "[?]";
            break;
        }
        return str;
    }

    static LogLevel::Level String2Level(const std::string& lev)
    {
        if (strcasecmp(lev.c_str(), "debug") == 0) {
            return LEVEL_DEBUG;
        }
        if (strcasecmp(lev.c_str(), "info") == 0) {
            return LEVEL_INFO;
        }
        if (strcasecmp(lev.c_str(), "warn") == 0) {
            return LEVEL_WARN;
        }
        if (strcasecmp(lev.c_str(), "error") == 0) {
            return LEVEL_ERROR;
        }
        if (strcasecmp(lev.c_str(), "fatal") == 0) {
            return LEVEL_FATAL;
        }
        return UNKNOW;
    }
};

} // namespace eular
#endif // #ifndef __LOG_LEVEL_H__