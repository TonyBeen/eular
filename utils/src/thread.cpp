/*************************************************************************
    > File Name: thread.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 10时06分39秒
 ************************************************************************/

// #define _DEBUG

#include "utils/thread.h"

#include <string.h>

#include <pthread.h>

#include "utils/errors.h"
#include "utils/exception.h"
#include "thread.h"

#define CAST2UINT(x) static_cast<uint32_t>(x)

namespace eular {
struct ThreadBase::ThreadImpl {
    uint32_t            _tid{};
    pthread_t           _pthread_tid{};
    Sem                 _sem_block; // 阻塞线程结束
    Sem                 _sem_wait;  // 用于等待线程创建完毕

    std::atomic<uint32_t>   _th_status{};
    std::atomic<bool>       _exit_status{};

    ThreadImpl() :
        _tid(0),
        _sem_block(0),
        _sem_wait(0),
        _th_status(CAST2UINT(ThreadStatus::THREAD_EXIT)),
        _exit_status(false)
    {
    }
};

ThreadBase::ThreadBase(const String8 &threadName) :
    userData(nullptr),
    mThreadName(threadName),
    mImpl(std::unique_ptr<ThreadImpl>(new ThreadImpl))
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
    return mImpl->_th_status;
}

void ThreadBase::stop()
{
    if (mImpl->_th_status == CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return;
    }

    mImpl->_exit_status = true;
    if (mImpl->_th_status == CAST2UINT(ThreadStatus::THREAD_WAITING)) { // 如果线程处于等待用户状态，则需要通知线程
        mImpl->_sem_block.post();
    }
}

bool ThreadBase::forceExit()
{
    if (mImpl->_th_status == CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return true;
    }

    mImpl->_exit_status = true;
    pthread_cancel(mImpl->_pthread_tid);
    int status = pthread_join(mImpl->_pthread_tid, nullptr);
    if (status) {
        String8 msg = String8::Format("pthread_join error. [%d,%s]", status, strerror(status));
        throw eular::Exception(msg);
    }

    mImpl->_th_status = CAST2UINT(ThreadStatus::THREAD_EXIT);
    return status == 0;
}

int32_t ThreadBase::getTid() const
{
    return mImpl->_tid;
}

void ThreadBase::detach()
{
    pthread_detach(mImpl->_pthread_tid);
}

void ThreadBase::join()
{
    pthread_join(mImpl->_pthread_tid, nullptr);
}

bool ThreadBase::ShouldExit()
{
    return mImpl->_exit_status;
}

int ThreadBase::run(size_t stackSize)
{
    if (mImpl->_th_status != CAST2UINT(ThreadStatus::THREAD_EXIT)) {
        return STATUS(INVALID_OPERATION);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize) {
        pthread_attr_setstacksize(&attr, stackSize);
    }

    int ret = pthread_create(&mImpl->_pthread_tid, &attr, threadloop, this);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        throw Exception(String8::Format("pthread_create error %s\n", strerror(ret)));
    }
    mImpl->_sem_wait.timedwait(1000); // 等待线程启动完毕

    return ret;
}

int32_t ThreadBase::start(size_t stackSize)
{
    int32_t code = 0;
    switch (mImpl->_th_status) {
    case CAST2UINT(ThreadStatus::THREAD_WAITING):
        mImpl->_sem_block.post();
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
    threadBase->mImpl->_tid = gettid();
    threadBase->mImpl->_sem_wait.post();
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);    // 设置任何时间点都可以取消线程

    while (threadBase->ShouldExit() == false) {
        threadBase->mImpl->_th_status = CAST2UINT(ThreadStatus::THREAD_RUNNING);
        int result = threadBase->threadWorkFunction(threadBase->userData);
        if (result == CAST2UINT(ThreadStatus::THREAD_EXIT) || threadBase->ShouldExit()) {
            break;
        }

        threadBase->mImpl->_th_status = CAST2UINT(ThreadStatus::THREAD_WAITING);

        threadBase->mImpl->_sem_block.wait();     // 阻塞线程，由用户决定下一次执行任务的时间
    }

    threadBase->mImpl->_th_status = CAST2UINT(ThreadStatus::THREAD_EXIT);
    return 0;
}

static thread_local Thread*         gLocalThread = nullptr;
static thread_local eular::String8  gThreadName;

struct Thread::ThreadImpl {
    int32_t     _tid;
    pthread_t   _pthread_tid{};
};

Thread::Thread(std::function<void()> callback, const String8 &threadName) :
    mImpl(std::unique_ptr<ThreadImpl>(new ThreadImpl)),
    mThreadName(threadName.length() ? threadName : "Unknow"),
    mCallback(callback),
    mShouldJoin(true),
    mSemaphore(0)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int status = pthread_create(&mImpl->_pthread_tid, &attr, &Thread::entrance, this);
    pthread_attr_destroy(&attr);
    if (status) {
        String8 msg = String8::Format("pthread_create error. [%d,%s]", status, strerror(status));
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
        pthread_setname_np(gLocalThread->mImpl->_pthread_tid, name.c_str());
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

int32_t Thread::getTid() const
{
    return mImpl->_tid;
}

void Thread::detach()
{
    if (mShouldJoin) {
        pthread_detach(mImpl->_pthread_tid);
        mShouldJoin = false;
    }
}

void Thread::join()
{
    if (mShouldJoin) {
        int ret = pthread_join(mImpl->_pthread_tid, nullptr);
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
    gLocalThread->mImpl->_tid = gettid();
    gLocalThread->mSemaphore.post();

    pthread_setname_np(pthread_self(), th->mThreadName.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(th->mCallback);

    cb();
    return 0;
}

} // namespace eular
