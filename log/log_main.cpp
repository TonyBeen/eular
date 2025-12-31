#include "log_main.h"
#include <mutex>
#include <memory>

static pthread_once_t gOnceFlag = PTHREAD_ONCE_INIT;
static eular::LogManager*   gLogManager = nullptr;

namespace eular {
LogManager::LogManager()
{
    pthread_mutex_init(&mListMutex, nullptr);
    mLogWriteList.push_back(new StdoutLogWrite());
    ::atexit(deleteInstance);
}

LogManager::~LogManager()
{
    if (mLogWriteList.size() > 0) {
        for (LogWriteIt it = mLogWriteList.begin(); it != mLogWriteList.end();) {
            if (*it != nullptr) {
                delete *it;
                it = mLogWriteList.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void LogManager::setPath(const std::string &path)
{
    mBasePath = path;
}

/**
 * @brief 非线程安全，禁止在程序运行后在添加日志节点，加锁影响性能
 * 
 * @param event 
 */
void LogManager::WriteLog(LogEvent *event)
{
    // pthread_mutex_lock(&mListMutex);
    for (LogWriteIt it = mLogWriteList.begin(); it != mLogWriteList.end(); ++it) {
        if (*it != nullptr) {
            if ((*it)->type() != static_cast<uint16_t>(OutputType::STDOUT)) {
                event->enableColor = false;
            }

            if ((*it)->type() == static_cast<uint16_t>(OutputType::CONSOLEOUT)) {
                (*it)->WriteToFile(*event);
            } else {
                std::string log = LogFormat::Format(event);
                (*it)->WriteToFile(log);
            }
        }
    }
    // pthread_mutex_unlock(&mListMutex);
}

const std::list<LogWrite *> &LogManager::GetLogWrite() const
{
    return mLogWriteList;
}

void LogManager::addLogWriteToList(int type)
{
    LogWrite *logWrite = nullptr;

    pthread_mutex_lock(&mListMutex);
    for (LogWriteIt it = mLogWriteList.begin(); it != mLogWriteList.end();) {
        if (*it == nullptr) {           // new 出来的对象为空
            it = mLogWriteList.erase(it);
            continue;
        }
        if ((*it)->type() == type) {    // 如果已存在则直接返回
            goto unlock;
        }
        ++it;
    }

    switch (static_cast<OutputType>(type)) {
        case OutputType::STDOUT:
            logWrite = new StdoutLogWrite();
            break;
        case OutputType::FILEOUT:
            logWrite = new FileLogWrite();
            logWrite->setBasePath(mBasePath);
            break;
        case OutputType::CONSOLEOUT:
            logWrite = new ConsoleLogWrite();
            break;
        default:
            goto unlock;
    }

    mLogWriteList.push_back(logWrite);

unlock:
    pthread_mutex_unlock(&mListMutex);
}

void LogManager::delLogWriteFromList(int type)
{
    pthread_mutex_lock(&mListMutex);
    for (LogWriteIt it = mLogWriteList.begin(); it != mLogWriteList.end();) {
        if (*it == nullptr) {
            it = mLogWriteList.erase(it);
            continue;
        }
        if ((*it)->type() == type) {
            delete (*it);
            *it = nullptr;      // erase出错导致未删除此处节点时起作用
            mLogWriteList.erase(it);
            break;
        }
        ++it;
    }
    pthread_mutex_unlock(&mListMutex);
}

void LogManager::once_entry()
{
    gLogManager = new (std::nothrow) LogManager();
}

LogManager *LogManager::getInstance()
{
    pthread_once(&gOnceFlag, once_entry);
    return gLogManager;
}

void LogManager::deleteInstance()
{
    if (gLogManager != nullptr) {
        delete gLogManager;
        gLogManager = nullptr;
    }
}

}