/*************************************************************************
    > File Name: utp_log.h
    > Author: hsz
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:47:35 PM CST
 ************************************************************************/

#ifndef __UTP_LOG_H__
#define __UTP_LOG_H__

#include <string.h>

#include "utp/logger.h"

#ifdef OS_WINDOWS
#define DIR_SEPARATOR       '\\'
#define DIR_SEPARATOR_STR   "\\"
#else
#define DIR_SEPARATOR       '/'
#define DIR_SEPARATOR_STR   "/"
#endif

#ifndef __FILENAME__
#define __FILENAME__  (strrchr(DIR_SEPARATOR_STR __FILE__, DIR_SEPARATOR) + 1)
#endif

#define UTP_LOGD(fmt, ...)  eular::utp::UtpLog(UTP_LOG_DEBUG, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGI(fmt, ...)  eular::utp::UtpLog(UTP_LOG_INFO, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGW(fmt, ...)  eular::utp::UtpLog(UTP_LOG_WARN, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGE(fmt, ...)  eular::utp::UtpLog(UTP_LOG_ERROR, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGF(fmt, ...)  eular::utp::UtpLog(UTP_LOG_FATAL, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

namespace eular {
namespace utp {

void UtpLog(int32_t level, const char *fileName, const char *funcName, int32_t line, const char* fmt, ...);

} // namespace utp
} // namespace eular

#endif // __UTP_LOG_H__
