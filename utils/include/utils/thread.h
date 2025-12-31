/*************************************************************************
    > File Name: thread.h
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 10时06分34秒
 ************************************************************************/

#ifndef __THREAD_H__
#define __THREAD_H__

#include <stdint.h>
#include <atomic>
#include <functional>
#include <memory>

#include <utils/string8.h>
#include <utils/mutex.h>
#include <utils/condition.h>
#include <utils/utils.h>

namespace eular {
class UTILS_API ThreadBase
{
    DISALLOW_COPY_AND_ASSIGN(ThreadBase);
public:
    enum ThreadStatus {
        THREAD_EXIT,
        THREAD_BLOCK,
        THREAD_RUNNING,
        THREAD_WAITING,
    };

    ThreadBase(const String8 &threadName);
    virtual ~ThreadBase();

    uint32_t        threadStatus() const;

    int32_t         start(size_t stackSize = 0);
    void            stop();
    bool            forceExit();
    const String8&  threadName() const { return mThreadName; }
    int32_t         getTid() const;

    void            detach();
    void            join();

protected:
    int             run(size_t stackSize);
    void*           userData;       // 用户传参数据块
    virtual int     threadWorkFunction(void *arg) = 0;
    String8         mThreadName;

private:
    static  void*   threadloop(void *user);
            bool    ShouldExit();

private:
    struct ThreadImpl;
    std::unique_ptr<ThreadImpl>     mImpl;
};

class UTILS_API Thread
{
public:
    using SP = std::shared_ptr<Thread>;

    Thread(std::function<void()> callback, const String8 &threadName = "");
    ~Thread();

    static void         SetName(const eular::String8 &name);
    static String8      GetName();
    static Thread *     GetThis();
    eular::String8      getName() const { return mThreadName; }
    int32_t             getTid() const;

    void detach();
    void join();

protected:
    static void *entrance(void *arg);

private:
    struct ThreadImpl;
    std::unique_ptr<ThreadImpl>     mImpl;
    eular::String8                  mThreadName;    // 线程名字
    std::function<void()>           mCallback;      // 线程执行函数
    uint8_t                         mShouldJoin;    // 1为由用户回收线程，0为自动回收
    eular::Sem                      mSemaphore;
};

} // namespace eular

#endif // __THREAD_H__