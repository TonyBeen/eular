/*************************************************************************
    > File Name: timer.h
    > Author: hsz
    > Mail:
    > Created Time: Thu 16 Sep 2021 02:32:45 PM CST
 ************************************************************************/

#ifndef __TIMER_H__
#define __TIMER_H__

#include <stdint.h>
#include <set>
#include <memory>
#include <functional>
#include <vector>

#include <utils/sysdef.h>
#include <utils/mutex.h>
#include <utils/thread.h>
#include <utils/singleton.h>

using std::set;
using std::function;
using std::shared_ptr;

namespace eular {
class TimerManager;
class Timer : public std::enable_shared_from_this<Timer>
{
public:
    typedef std::shared_ptr<Timer> sp;
    typedef std::function<void(void)> CallBack;
    ~Timer();

    Timer &operator=(const Timer& timer);

    void setNextTime(uint64_t timeMs) { mTime = timeMs; }
    void setCallback(CallBack cb) { mCb = cb; }
    void setRecycleTime(uint64_t ms) { mRecycleTime = ms; }

    void cancel();
    void resetCanceled();
    void refresh();

    /**
     * @param ms        下一次执行时间(相对时间：当前时间戳 + ms为下一次执行时间)
     * @param recycle   是否循环
     */
    void reset(uint64_t ms, CallBack cb, uint32_t recycle);

private:
    Timer();
    Timer(uint64_t ms, CallBack cb, uint32_t recycle);
    Timer(const Timer& timer);

private:
    friend class TimerManager;
    struct Comparator {
        // 传给set的比较器，从小到大排序
        bool operator()(Timer* const &l, Timer* const &r) const {
            if (l == nullptr && r == nullptr) {
                return false;
            }
            if (l == nullptr) {
                return true;
            }
            if (r == nullptr) {
                return false;
            }
            if (l->mTime == r->mTime) { // 时间相同，比较ID
                return l->mUniqueId < r->mUniqueId;
            }
            return l->mTime < r->mTime;
        }

        bool operator()(Timer* &l, Timer* &r) {
            if (l == nullptr && r == nullptr) {
                return false;
            }
            if (l == nullptr) {
                return true;
            }
            if (r == nullptr) {
                return false;
            }
            if (l->mTime == r->mTime) { // 时间相同，比较ID
                return l->mUniqueId < r->mUniqueId;
            }
            return l->mTime < r->mTime;
        }
    };

private:
    uint64_t            mTime;          // (绝对时间)下一次执行时间(ms)
    uint64_t            mRecycleTime;   // 循环时间ms
    CallBack            mCb;            // 回调函数
    uint64_t            mUniqueId;      // 定时器唯一ID
    std::atomic<bool>   mCanceled;      // 是否取消
    class TimerManager *mTimerManager;
};

class TimerManager : public ThreadBase
{
    DISALLOW_COPY_AND_ASSIGN(TimerManager);
public:
    typedef std::shared_ptr<TimerManager> SP;
    typedef std::unique_ptr<TimerManager> Ptr;
    typedef std::set<Timer *, Timer::Comparator>::iterator TimerIterator;

    TimerManager();
    ~TimerManager();

    int startTimer(bool useCallerThread = false);
    void stopTimer();

    const Timer *getNearTimer() { return *(mTimers.begin()); }
    uint64_t addTimer(uint64_t ms, Timer::CallBack cb, uint32_t recycle = 0);
    bool delTimer(uint64_t uniqueId);

protected:
    virtual int threadWorkFunction(void *arg) override;
    void ListExpireTimer();
    void addTimer(Timer *timer);
    void onNotify();

private:
    Sem     mSignal;
    RWMutex mRWMutex;
    int32_t mSockPair[2];

    std::atomic<bool> mShouldExit;
    std::vector<Timer *> mExpireTimerVec;
    std::set<Timer *, Timer::Comparator>  mTimers;        // 定时器集合
    friend class Singleton<TimerManager>;
};

} // namespace eular

#endif // __TIMER_H__
