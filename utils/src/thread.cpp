/*************************************************************************
    > File Name: thread.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 10时06分39秒
 ************************************************************************/

// #define _DEBUG

#include "utils/thread.h"

#include <string.h>

#include "utils/errors.h"
#include "utils/exception.h"

namespace eular {
ThreadBase::ThreadBase(const String8 &threadName) :
    userData(nullptr),
    mThreadName(threadName),
    mPid(0),
    mKernalTid(0),
    mTid(0),
    mSem(0),
    mThreadStatus(THREAD_EXIT),
    mExitStatus(false)
{
}

ThreadBase::~ThreadBase()
{
    if (threadStatus() != THREAD_EXIT) { // 等待线程退出，否则在析构完成之后会导致线程段错误问题
        stop();
    }
}

uint32_t ThreadBase::threadStatus() const
{
    return mThreadStatus;
}

void ThreadBase::stop()
{
    if (mThreadStatus == THREAD_EXIT) {
        return;
    }

    mExitStatus = true;
    if (mThreadStatus == THREAD_WAITING) { // 如果线程处于等待用户状态，则需要通知线程
        mSem.post();
    }
}

bool ThreadBase::forceExit()
{
    if (mThreadStatus == THREAD_EXIT) {
        return true;
    }

    bool flag = pthread_cancel(mTid) == 0;
    mExitStatus = THREAD_EXIT;
    return flag;
}

bool ThreadBase::ShouldExit()
{
    return mExitStatus;
}

int ThreadBase::run(size_t stackSize)
{
    if (mThreadStatus != THREAD_EXIT) {
        return INVALID_OPERATION;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize) {
        pthread_attr_setstacksize(&attr, stackSize);
    }
    // pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int ret = pthread_create(&mTid, &attr, threadloop, this);
    if (ret == 0) {
        mThreadStatus = THREAD_RUNNING;
    } else {
        throw Exception(String8::format("pthread_create error %s\n", strerror(ret)));
    }

    pthread_detach(mTid);
    pthread_attr_destroy(&attr);
    return ret;
}

void ThreadBase::start()
{
    switch (mThreadStatus) {
    case THREAD_WAITING:
        mSem.post();
        break;
    case THREAD_RUNNING:
        break;
    case THREAD_EXIT:
        run();
        break;
    default:
        break;
    }
}

void *ThreadBase::threadloop(void *user)
{
    ThreadBase *threadBase = (ThreadBase *)user;
    threadBase->mKernalTid = gettid();
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);    // 设置任何时间点都可以取消线程

    while (threadBase->ShouldExit() == false) {
        int result = threadBase->threadWorkFunction(threadBase->userData);
        if (result == THREAD_EXIT || threadBase->ShouldExit()) {
            break;
        }

        threadBase->mThreadStatus = THREAD_WAITING;

        threadBase->mSem.wait();     // 阻塞线程，由用户决定下一次执行任务的时间
        threadBase->mThreadStatus = THREAD_RUNNING;
    }

    threadBase->mExitStatus = THREAD_EXIT;
    return 0;
}

static thread_local Thread *gLocalThread = nullptr;
static thread_local eular::String8 gThreadName;

Thread::Thread(std::function<void()> callback, const String8 &threadName) :
    mKernalTid(0),
    mTid(0),
    mThreadName(threadName.length() ? threadName : "Unknow"),
    mCallback(callback),
    mShouldJoin(true),
    mSemaphore(0)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int ret = pthread_create(&mTid, &attr, &Thread::entrance, this);
    pthread_attr_destroy(&attr);
    if (ret) {
        String8 msg = String8::format("pthread_create error. [%d,%s]", errno, strerror(errno));
        throw eular::Exception(msg);
    }
    mSemaphore.wait();
}

Thread::~Thread()
{
    if (mShouldJoin && mTid) {
        pthread_detach(mTid);
    }
}

void Thread::SetName(eular::String8 name)
{
    if (name.empty()) {
        return;
    }
    if (gLocalThread) {
        gLocalThread->mThreadName = name;
    }
    gThreadName = name;
}

String8 Thread::GetName()
{
    return gThreadName;
}

Thread *Thread::GetThis()
{
    return gLocalThread;
}

void Thread::detach()
{
    if (mTid) {
        pthread_detach(mTid);
        mShouldJoin = false;
    }
}

void Thread::join()
{
    if (mShouldJoin && mTid) {
        int ret = pthread_join(mTid, nullptr);
        if (ret) {
            String8 msg = String8::format("pthread_join error. [%d,%s]", errno, strerror(errno));
            throw eular::Exception(msg);
        }
        mTid = 0;
    }
    mShouldJoin = false;
}

void *Thread::entrance(void *arg)
{
    Thread *th = static_cast<Thread *>(arg);
    gLocalThread = th;
    gThreadName = th->mThreadName;
    gLocalThread->mKernalTid = gettid();
    gLocalThread->mSemaphore.post();

    pthread_setname_np(pthread_self(), th->mThreadName.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(th->mCallback);

    cb();
    return 0;
}

} // namespace eular
