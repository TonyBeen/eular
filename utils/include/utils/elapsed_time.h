/*************************************************************************
    > File Name: elapsed_time.h
    > Author: hsz
    > Brief:
    > Created Time: Sun 25 Feb 2024 03:13:51 PM CST
 ************************************************************************/

#ifndef __UTILSE_ELAPSED_TIME_H__
#define __UTILSE_ELAPSED_TIME_H__

#include <chrono>
#include <list>

#include <utils/sysdef.h>

namespace eular {
enum class ElapsedTimeType {
    SECOND,         // 秒
    MILLISECOND,    // 毫秒
    MICROSECOND,    // 微秒
    NANOSECOND,     // 纳秒
};

class UTILS_API ElapsedTime
{
public:
    ElapsedTime();
    ElapsedTime(ElapsedTimeType type);
    ~ElapsedTime();

    // 计时开始
    void start();

    // 计时结束
    void stop();

    // 获取消耗的时间, 失败返回UINT64_MAX
    uint64_t elapsedTime();

    // 重置
    void reset();

protected:
    uint64_t getCurrentTime() const;

private:
    using TimePair = std::pair<uint64_t, uint64_t>;

    ElapsedTimeType         mTimeType;
    uint64_t                mBeginTime;
    std::list<TimePair>     mElapsedTimeList;
};

} // namespace eular

#endif // __UTILSE_ELAPSED_TIME_H__
