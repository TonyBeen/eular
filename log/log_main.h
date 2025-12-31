/*************************************************************************
    > File Name: log_main.h
    > Author: hsz
    > Desc: This file is written to handle log events
    > Created Time: 2021年04月12日 星期一 22时15分42秒
 ************************************************************************/

#ifndef __LOG_MAIN_H__
#define __LOG_MAIN_H__

#include "log_event.h"
#include "log_level.h"
#include "log_write.h"
#include "log_format.h"
#include <memory>
#include <queue>
#include <pthread.h>
#include <list>

#define MAX_QUEUE_SIZE (1024 * 10)

namespace eular {
class LogManager {
public:
    typedef std::list<LogWrite *>::iterator LogWriteIt;
    ~LogManager();
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    void setPath(const std::string &path);
    void WriteLog(LogEvent *event);
    static LogManager *getInstance();
    static void deleteInstance();

    const std::list<LogWrite *> &GetLogWrite() const;
    void addLogWriteToList(int type);
    void delLogWriteFromList(int type);

private:
    static void once_entry();
    LogManager();

private:
    std::list<LogWrite *>   mLogWriteList;
    std::string             mBasePath;
    pthread_mutex_t         mListMutex;
};
} // namespace eular
#endif // __LOG_MAIN_H__