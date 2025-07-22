/*************************************************************************
    > File Name: time.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月18日 星期五 15时47分12秒
 ************************************************************************/

#ifndef __UTILS_TIME_H__
#define __UTILS_TIME_H__

#include <stdint.h>
#include <string>

namespace eular {
class Time
{
public:
    /**
     * @brief 获取系统时间
     *
     * @return uint64_t 返回系统时间(ms)
     */
    static uint64_t SystemTime();

    /**
     * @brief 获取绝对时间
     *
     * @return uint64_t 返回绝对时间(ms)
     */
    static uint64_t AbsTime();

    /**
     * @brief 获取GMT时间
     *
     * @param tim 时间指针
     * @return std::string 返回GMT时间字符串
     */
    static std::string GmtTime(const time_t *tim = nullptr);

    /**
     * @brief 格式化时间
     *
     * @param time 时间
     * @param format 格式化字符串
     * @return std::string 返回格式化后的时间字符串
     */
    static std::string Format(time_t time, const std::string &format = "%Y-%m-%d %H:%M:%S");
    static std::string Format(time_t time, const char *format = "%Y-%m-%d %H:%M:%S");
};

} // namespace eular

#endif // __UTILS_TIME_H__
