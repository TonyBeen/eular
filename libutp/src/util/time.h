/*************************************************************************
    > File Name: time.h
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 03:31:29 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_TIME_H__
#define __UTP_UTIL_TIME_H__

#include <stdint.h>
#include <atomic>

#include "utp/platform.h"
#include "commom.h"

namespace eular {
namespace utp {
namespace time {

// 获取绝对时间, 不受系统时间影响. 单位: 毫秒
uint64_t MonotonicMs() noexcept;

// 获取绝对时间, 不受系统时间影响. 单位: 微秒
uint64_t MonotonicUs() noexcept;

// 获取UNIX毫秒时间戳. 单位: 毫秒
uint64_t RealtimeMs() noexcept;

/**
 * @brief 获取当前地区的时区
 * @note 时区的取值范围为[-12, 12], 表示与UTC(格林尼治标准时间)的时差
 * @note 例如: 中国的时区为东八区 utc + 8, 美国的时区为西五区 utc - 5 // refer https://www.beijing-time.org/shiqu/
 *
 * @return int32_t 时区, 失败返回 INT32_MIN
 */
int32_t  Timezone() noexcept;

} // namespace time
} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_TIME_H__
