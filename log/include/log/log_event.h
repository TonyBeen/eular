/*************************************************************************
    > File Name: log_event.h
    > Author: hsz
    > Desc:
    > Created Time: 2021年04月12日 星期一 22时15Desc
 ************************************************************************/

#ifndef __LOG_EVENT_H__
#define __LOG_EVENT_H__

#include "log_level.h"
#include <time.h> 
#include <sys/time.h> 
#include <sys/types.h>

#define LOG_TAG_SIZE (64)

namespace eular {
struct LogEvent {
    struct timeval  time;       // 时间
    int             pid;        // 进程ID
    pthread_t       tid;        // 线程ID
    LogLevel::Level level;      // 日志级别
    char            tag[LOG_TAG_SIZE];  // tag
    char *          msg;        // 日志消息
    bool            enableColor;// 是否启用颜色
};

static inline LogEvent LogEventDump(const LogEvent *ev)
{
    LogEvent ret;
    memcpy(&ret, ev, sizeof(LogEvent));
    ret.msg = (char *)malloc(strlen(ev->msg) + 1);
    if (ret.msg != nullptr) {
        memcpy(ret.msg, ev->msg, strlen(ev->msg));
    }

    return ret;
}

} // namespace eular
#endif // __LOG_EVENT_H__