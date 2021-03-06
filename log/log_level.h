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
class LogLevel {
public:
    enum Level {
        UNKNOW = -1,
        DEBUG = 0,
        INFO  = 1,
        WARN  = 2,
        ERROR = 3,
        FATAL = 4
    };
    static std::string ToString(Level l)
    {
        std::string str;
        switch (l) {
        case DEBUG:
            str = "DEBUG";
            break;
        case INFO:
            str = "INFO";
            break;
        case WARN:
            str = "WARN";
            break;
        case ERROR:
            str = "ERROR";
            break;
        case FATAL:
            str = "FATAL";
            break;
        
        default:
            str = "";
            break;
        }
        return str;
    }

    static std::string ToFormatString(Level level)
    {
        std::string str;
        switch (level) {
        case DEBUG:
            str = "[D]";
            break;
        case INFO:
            str = "[I]";
            break;
        case WARN:
            str = "[W]";
            break;
        case ERROR:
            str = "[E]";
            break;
        case FATAL:
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
            return DEBUG;
        }
        if (strcasecmp(lev.c_str(), "info") == 0) {
            return INFO;
        }
        if (strcasecmp(lev.c_str(), "warn") == 0) {
            return WARN;
        }
        if (strcasecmp(lev.c_str(), "error") == 0) {
            return ERROR;
        }
        if (strcasecmp(lev.c_str(), "fatal") == 0) {
            return FATAL;
        }
        return UNKNOW;
    }
};

} // namespace eular
#endif // #ifndef __LOG_LEVEL_H__