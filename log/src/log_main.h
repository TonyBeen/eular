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
#include "log_format.h"
#include <queue>
#include <pthread.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#define MAX_QUEUE_SIZE (1024 * 10)

namespace eular {
class LogManager {
public:
    ~LogManager();
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    void setPath(const std::string &path, const std::string &fileStem);
    void setFileRotation(uint64_t maxFileSize, uint32_t maxFileCount);
    void WriteLog(const LogEvent *event);
    void Flush();
    static LogManager *getInstance();
    static void deleteInstance();

    void addLogWriteToList(int type);
    void delLogWriteFromList(int type);

private:
    struct QueuedLog {
        std::string plain;
        std::string console;
    };

    static void once_entry();
    LogManager();
    void workerLoop();
    bool ensureFileOpened();
    void rotateFileIfNeeded(size_t incoming);
    std::string buildActiveLogPath() const;
    std::string buildArchiveLogPath(uint32_t index) const;
    std::string buildUnlimitedArchiveLogPath();
    std::string resolveBasePath() const;

private:
    std::atomic<bool>               mRunning;
    std::atomic<uint32_t>           mOutputMask;
    std::atomic<uint64_t>           mDropped;
    std::queue<QueuedLog>           mQueue;
    std::mutex                      mQueueMutex;
    std::condition_variable         mQueueCv;
    std::thread                     mWorker;
    mutable std::mutex              mPathMutex;
    std::string                     mBasePath;
    std::string                     mFileStem;
    std::atomic<uint64_t>           mMaxFileSize;
    std::atomic<uint32_t>           mMaxFileCount;
    std::atomic<bool>               mReopenFile;
    int                             mFileFd;
    uint64_t                        mFileSize;
    uint64_t                        mRotateSequence;
};
} // namespace eular
#endif // __LOG_MAIN_H__