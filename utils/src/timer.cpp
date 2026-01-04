/*************************************************************************
    > File Name: timer.cpp
    > Author: hsz
    > Mail:
    > Created Time: Thu 16 Sep 2021 02:33:01 PM CST
 ************************************************************************/

// #define _DEBUG
#include "utils/timer.h"

#include <assert.h>
#include <time.h>
#include <atomic>

#include <fcntl.h>

#if defined(OS_LINUX)
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#elif defined(OS_MACOS)
#include <sys/event.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#elif defined(OS_WINDOWS)
#include <winsock2.h>
typedef int     socklen_t;
#endif

#include "utils/exception.h"
#include "utils/errors.h"
#include "utils/time.h"

#if defined(OS_LINUX) || defined(OS_MACOS)
inline int closesocket(int fd) {
    return close(fd);
}
#endif

#define WR_SOCK_IDX 0
#define RD_SOCK_IDX 1

namespace eular {
static std::atomic<uint64_t> gTimerCount = {0};

int SocketPair(int32_t family, int32_t type, int32_t protocol, int sv[2]) {
#if defined(OS_LINUX) || defined(OS_MACOS)
    return socketpair(AF_LOCAL, type, protocol, sv);
#endif
    if (family != AF_INET || type != SOCK_STREAM) {
        return -1;
    }

    int listenfd, connfd, acceptfd;
    listenfd = connfd = acceptfd = -1;
    struct sockaddr_in localaddr;
    socklen_t addrlen = sizeof(localaddr);
    memset(&localaddr, 0, addrlen);
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    localaddr.sin_port = 0;
    // listener
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        goto error;
    }
    if (bind(listenfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("bind");
        goto error;
    }
    if (listen(listenfd, 1) < 0) {
        perror("listen");
        goto error;
    }
    if (getsockname(listenfd, (struct sockaddr*)&localaddr, &addrlen) < 0) {
        perror("getsockname");
        goto error;
    }
    // connector
    connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        goto error;
    }
    if (connect(connfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("connect");
        goto error;
    }
    // acceptor
    acceptfd = accept(listenfd, (struct sockaddr*)&localaddr, &addrlen);
    if (acceptfd < 0) {
        perror("accept");
        goto error;
    }

    closesocket(listenfd);
    sv[0] = connfd;
    sv[1] = acceptfd;
    return 0;

error:
    if (listenfd != -1) {
        closesocket(listenfd);
    }
    if (connfd != -1) {
        closesocket(connfd);
    }
    if (acceptfd != -1) {
        closesocket(acceptfd);
    }

    return -1;
}

Timer::Timer(uint64_t ms, CallBack cb, uint32_t recycle) :
    mTime(0),
    mRecycleTime(recycle),
    mCb(cb),
    mUniqueId(++gTimerCount),
    mCanceled(false),
    mTimerManager(nullptr)
{
    mTime = Time::AbsTime() + ms;
}

Timer::Timer(const Timer& timer) :
    std::enable_shared_from_this<Timer>(),
    mTime(timer.mTime),
    mRecycleTime(timer.mRecycleTime),
    mCb(timer.mCb),
    mUniqueId(timer.mUniqueId),
    mCanceled(timer.mCanceled.load(std::memory_order_relaxed)),
    mTimerManager(timer.mTimerManager)
{
}

Timer::~Timer()
{
}

Timer &Timer::operator=(const Timer& timer)
{
    if (this != &timer) {
        mUniqueId = timer.mUniqueId;
        mTime = timer.mTime;
        mRecycleTime = timer.mRecycleTime;
        mCb = timer.mCb;
        mCanceled = timer.mCanceled.load(std::memory_order_relaxed);
        mTimerManager = timer.mTimerManager;
    }

    return *this;
}

/**
 * @brief 取消执行回调
 */
void Timer::cancel()
{
    mCanceled.store(true, std::memory_order_relaxed);
}

void Timer::resetCanceled()
{
    mCanceled.store(false, std::memory_order_relaxed);
}

/**
 * @brief 刷新下次执行时间，仅当设置循环生效
 */
void Timer::refresh()
{
    if (mRecycleTime > 0) {
        mTime = Time::AbsTime() + mRecycleTime;
    }
}

/**
 * @brief 重新设置执行时间，回调，循环执行时间
 */
void Timer::reset(uint64_t ms, CallBack cb, uint32_t recycle)
{
    mTime = Time::AbsTime() + ms;
    mCb = cb;
    mRecycleTime = recycle;
    mCanceled.store(false, std::memory_order_relaxed);
}

TimerManager::TimerManager() :
    ThreadBase("timer thread"),
    mSignal(0),
    mShouldExit(false)
{
    mSockPair[0] = -1;
    mSockPair[1] = -1;
    int32_t status = SocketPair(AF_INET, SOCK_STREAM, 0, mSockPair);
    if (status < 0) {
#if defined(OS_WINDOWS)
        int32_t status = WSAGetLastError();
#else
        int32_t status = errno;
#endif
        throw Exception(String8::Format("create socket pair error. [%d, %s]", status, FormatErrno(status).c_str()));
    }

#if defined(OS_WINDOWS)
    u_long mode = 1;
    ioctlsocket(mSockPair[WR_SOCK_IDX], FIONBIO, &mode);
    ioctlsocket(mSockPair[RD_SOCK_IDX], FIONBIO, &mode);
#else
    fcntl(mSockPair[WR_SOCK_IDX], F_SETFL, fcntl(mSockPair[WR_SOCK_IDX], F_GETFL) |  O_NONBLOCK);
    fcntl(mSockPair[RD_SOCK_IDX], F_SETFL, fcntl(mSockPair[RD_SOCK_IDX], F_GETFL) |  O_NONBLOCK);
#endif
}

TimerManager::~TimerManager()
{
    stopTimer();

    mRWMutex.wlock();
    for (auto& it : mTimers) {
        if (it != nullptr) {
            delete it;
        }
    }
    mTimers.clear();
    mRWMutex.unlock();

    if (mSockPair[WR_SOCK_IDX] > 0) {
        closesocket(mSockPair[WR_SOCK_IDX]);
        mSockPair[WR_SOCK_IDX] = -1;
    }
    if (mSockPair[RD_SOCK_IDX] > 0) {
        closesocket(mSockPair[RD_SOCK_IDX]);
        mSockPair[RD_SOCK_IDX] = -1;
    }
}

int TimerManager::startTimer(bool useCallerThread)
{
    mUseCallerThread = useCallerThread;
    if (useCallerThread) {
        return threadWorkFunction(this);
    }

    return start();
}

void TimerManager::stopTimer()
{
    mShouldExit = true;
    mSignal.post();
    stop();

    onNotify();

    if (!mUseCallerThread) {
        join();
    }
}

/**
 * @brief   添加一个定时器
 * @param ms 延迟的时间，单位毫秒
 * @param cb 回调函数
 * @param arg 回调函数的入参
 * @param recycle 循环调用的时间，第一次调用为ms时，下一次为recycle毫秒后，循环
 * @return 返回定时器唯一ID，方便删除
 */
uint64_t TimerManager::addTimer(uint64_t ms, Timer::CallBack cb, uint32_t recycle)
{
    WRAutoLock<RWMutex> wrLock(mRWMutex);
    Timer *timer = new Timer(ms, cb, recycle);
    if (timer == nullptr) {
        return 0;
    }

    auto pair = mTimers.insert(timer);
    if (pair.second) {
        if (mTimers.size() == 1) {
            mSignal.post();
        }
        if (pair.first == mTimers.begin()) {
            onNotify();
        }
        return (*pair.first)->mUniqueId;
    }

    delete timer;
    return 0;
}

void TimerManager::addTimer(Timer *timer)
{
    if (timer == nullptr) {
        return;
    }
    WRAutoLock<RWMutex> lock(mRWMutex);
    mTimers.insert(timer);
}

void TimerManager::onNotify()
{
    send(mSockPair[WR_SOCK_IDX], "x", 1, 0); // 唤醒 select
}

bool TimerManager::delTimer(uint64_t uniqueId)
{
    WRAutoLock<RWMutex> wrLock(mRWMutex);
    bool flag = false;
    for (auto it = mTimers.begin(); it != mTimers.end();) {
        if ((*it)->mUniqueId == uniqueId) {
            mTimers.erase(it);
            flag = true;
            break;
        }
        ++it;
    }
    return flag;
}

void TimerManager::ListExpireTimer()
{
    uint64_t currTimeMs = Time::AbsTime();
    WRAutoLock<RWMutex> wrLock(mRWMutex);
    for (TimerManager::TimerIterator it = mTimers.begin(); it != mTimers.end(); ) {
        if ((*it)->mTime <= currTimeMs) {
            mExpireTimerVec.push_back(*it);
            it = mTimers.erase(it);
        } else {
            break;
        }
    }
}

int TimerManager::threadWorkFunction(void *arg)
{
    TimerManager *this_ = static_cast<TimerManager *>(arg);
    UNUSED(this_);

    fd_set readfds;
    TimerManager::TimerIterator it;
    int32_t n = 0;
    while (mShouldExit == false) {
        {
            RDAutoLock<RWMutex> lock(mRWMutex);
            if (mTimers.size() == 0) {
                mRWMutex.unlock();
                mSignal.wait();
                mRWMutex.rlock();
            }
            it = mTimers.begin();
            if (it == mTimers.end()) { // 当不存在定时器且调用stopTimer后需要校验
                continue;
            }
        }

        FD_ZERO(&readfds);
        FD_SET(mSockPair[RD_SOCK_IDX], &readfds); // 监听新连接
        uint64_t nextTime = (*it)->mTime - Time::AbsTime();
        if (nextTime > 0) {
            struct timeval tv = {(int32_t)nextTime / 1000, (int32_t)nextTime % 1000 * 1000};
            n = select(mSockPair[RD_SOCK_IDX] + 1, &readfds, nullptr, nullptr, &tv);
        }

        if (n < 0) {
            break;
        } else if (n > 0) {
            char buffer[64];
            recv(mSockPair[RD_SOCK_IDX], buffer, 64, 0);
        }

        ListExpireTimer();
        for (auto &vecIt : mExpireTimerVec) {
            if (vecIt->mCb != nullptr && !vecIt->mCanceled.load(std::memory_order_relaxed)) {
                try {
                    vecIt->mCb();
                } catch (...) {
                }
            }

            if (vecIt->mRecycleTime > 0 && vecIt->mCb) {
                vecIt->mTime += vecIt->mRecycleTime;
                addTimer(vecIt);
            } else {
                delete vecIt;
            }
        }
        mExpireTimerVec.clear();
    }

    return ThreadBase::ThreadStatus::THREAD_EXIT;
}

} // namespace eular
