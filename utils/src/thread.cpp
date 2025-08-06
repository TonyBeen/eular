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

#define CAST2UINT(x) static_cast<uint32_t>(x)

namespace eular {
ThreadBase::ThreadBase(const String8 &threadName) :
    userData(nullptr),
    mThreadName(threadName),
    mKernalTid(0),
    mSem(0),
    mSemWait(0),
    mThreadStatus(CAST2UINT(ThreadStatus::THREAD_EXIT)),
    mExitStatus(false)
{
}

ThreadBase::~ThreadBase()
{
    if (threadStatus() != CAST2UINT(ThreadStatus::THREAD_EXIT)) { // 等待线程退出，否则在析构完成之后会导致线程段错误问题
        stop();
    }
}

uint32_t ThreadBase::threadStatus() const
{
    return mThreadStatus;
}

void ThreadBase::stop()
{
    if (mThreadStatus == CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return;
    }

    mExitStatus = true;
    if (mThreadStatus == CAST2UINT(ThreadStatus::THREAD_WAITING)) { // 如果线程处于等待用户状态，则需要通知线程
        mSem.post();
    }
}

bool ThreadBase::forceExit()
{
    if (mThreadStatus == CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return true;
    }

    mExitStatus = true;
    pthread_cancel(mTid);
    int ret = pthread_join(mTid, nullptr);
    if (ret) {
        String8 msg = String8::Format("pthread_join error. [%d,%s]", errno, strerror(errno));
        throw eular::Exception(msg);
    }

    mThreadStatus = CAST2UINT(ThreadStatus::THREAD_EXIT);
    return ret == 0;
}

bool ThreadBase::ShouldExit()
{
    return mExitStatus;
}

int ThreadBase::run(size_t stackSize)
{
    if (mThreadStatus != CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return STATUS(INVALID_OPERATION);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize) {
        pthread_attr_setstacksize(&attr, stackSize);
    }

    int ret = pthread_create(&mTid, &attr, threadloop, this);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        throw Exception(String8::Format("pthread_create error %s\n", strerror(ret)));
    }
    mSemWait.timedwait(1000); // 等待线程启动完毕

    return ret;
}

int32_t ThreadBase::start(size_t stackSize)
{
    int32_t code = 0;
    switch (mThreadStatus) {
    case CAST2UINT(ThreadStatus::THREAD_WAITING):
        mSem.post();
        break;
    case CAST2UINT(ThreadStatus::THREAD_RUNNING):
        break;
    case CAST2UINT(ThreadStatus::THREAD_EXIT):
        code = run(stackSize);
        break;
    default:
        break;
    }

    return code;
}

void *ThreadBase::threadloop(void *user)
{
    ThreadBase *threadBase = (ThreadBase *)user;
    threadBase->mKernalTid = gettid();
    threadBase->mSemWait.post();
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);    // 设置任何时间点都可以取消线程

    while (threadBase->ShouldExit() == false) {
        threadBase->mThreadStatus = CAST2UINT(ThreadStatus::THREAD_RUNNING);
        int result = threadBase->threadWorkFunction(threadBase->userData);
        if (result == CAST2UINT(ThreadStatus::THREAD_EXIT) || threadBase->ShouldExit()) {
            break;
        }

        threadBase->mThreadStatus = CAST2UINT(ThreadStatus::THREAD_WAITING);

        threadBase->mSem.wait();     // 阻塞线程，由用户决定下一次执行任务的时间
    }

    threadBase->mThreadStatus = CAST2UINT(ThreadStatus::THREAD_EXIT);
    return 0;
}

static thread_local Thread *gLocalThread = nullptr;
static thread_local eular::String8 gThreadName;

Thread::Thread(std::function<void()> callback, const String8 &threadName) :
    mKernalTid(0),
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
        String8 msg = String8::Format("pthread_create error. [%d,%s]", errno, strerror(errno));
        throw eular::Exception(msg);
    }
    mSemaphore.timedwait(1000);
}

Thread::~Thread()
{
    join();
}

void Thread::SetName(const eular::String8 &name)
{
    if (name.empty()) {
        return;
    }
    if (gLocalThread) {
        gLocalThread->mThreadName = name;
        pthread_setname_np(gLocalThread->mTid, name.c_str());
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
    if (mShouldJoin) {
        pthread_detach(mTid);
        mShouldJoin = false;
    }
}

void Thread::join()
{
    if (mShouldJoin) {
        int ret = pthread_join(mTid, nullptr);
        if (ret) {
            String8 msg = String8::Format("pthread_join error. [%d,%s]", ret, strerror(ret));
            throw eular::Exception(msg);
        }
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
