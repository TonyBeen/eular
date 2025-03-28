#ifndef __KCP_TIME_H__
#define __KCP_TIME_H__

#include <stdint.h>

#include <kcp_def.h>

EXTERN_C_BEGIN

/**
 * @brief 获取绝对时间, 单位为毫秒
 */
uint64_t kcp_time_monotonic_ms();

/**
 * @brief 获取UNIX毫秒时间戳
 */
uint64_t kcp_time_realtime_ms();

/**
 * @brief 获取当前地区的时区
 * @note 时区的取值范围为[-12, 12]，表示与UTC(格林尼治标准时间)的时差
 * @note 例如，中国的时区为东八区 utc + 8，美国的时区为西五区 utc - 5 // refer https://www.beijing-time.org/shiqu/
 *
 * @return int32_t 时区
 */
int32_t kcp_time_zone();

EXTERN_C_END

#endif // __KCP_TIME_H__
