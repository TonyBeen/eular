/*************************************************************************
    > File Name: logger.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:47:35 PM CST
 ************************************************************************/

#ifndef __UTP_LOG_H__
#define __UTP_LOG_H__

#include <string.h>
#include <array>

#include "utp/platform.h"
#include "fmt/fmt.h"
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

#define LOG_BUFFER_SIZE 8192

#define UTP_LOGD(fmt, ...)  eular::utp::UtpLog(UTP_LOG_DEBUG, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGI(fmt, ...)  eular::utp::UtpLog(UTP_LOG_INFO, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGW(fmt, ...)  eular::utp::UtpLog(UTP_LOG_WARN, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGE(fmt, ...)  eular::utp::UtpLog(UTP_LOG_ERROR, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define UTP_LOGF(fmt, ...)  eular::utp::UtpLog(UTP_LOG_FATAL, __FILENAME__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

#define UTP_LOGD_FMT(format, ...)  eular::utp::UtpLogV(UTP_LOG_DEBUG, __FILENAME__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)
#define UTP_LOGI_FMT(format, ...)  eular::utp::UtpLogV(UTP_LOG_INFO, __FILENAME__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)
#define UTP_LOGW_FMT(format, ...)  eular::utp::UtpLogV(UTP_LOG_WARN, __FILENAME__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)
#define UTP_LOGE_FMT(format, ...)  eular::utp::UtpLogV(UTP_LOG_ERROR, __FILENAME__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)
#define UTP_LOGF_FMT(format, ...)  eular::utp::UtpLogV(UTP_LOG_FATAL, __FILENAME__, __FUNCTION__, __LINE__, format, ##__VA_ARGS__)

static void default_log_cb(int32_t level, const char *log, int32_t size);
utp_log_callback_t  g_log_cb = default_log_cb;
int32_t             g_log_level = UTP_LOG_SILENT;


namespace eular {
namespace utp {

void UtpLog(int32_t level, const char *fileName, const char *funcName, int32_t line, const char* fmt, ...);

template <typename... Args>
void UtpLogV(int32_t level, const char *fileName, const char *funcName, int32_t line, fmt::format_string<Args...> format_str, Args&&... args)
{
    if (level < g_log_level || !g_log_cb) {
        return;
    }

    static THREAD_LOCAL fmt::basic_memory_buffer<char, LOG_BUFFER_SIZE> buffer;
    buffer.clear();

    try {
        fmt::format_to(std::back_inserter(buffer), "[{}:{}:{}()] -> ", fileName, line, funcName);
        fmt::format_to(std::back_inserter(buffer), format_str, std::forward<Args>(args)...);
    } catch (const fmt::format_error& e) {
        buffer.clear();
        fmt::format_to(std::back_inserter(buffer), "[format error]: {}", e.what());
    } catch (... ) {
        buffer.clear();
    }

    g_log_cb(level, buffer.data(), static_cast<int32_t>(buffer.size()));
}

} // namespace utp
} // namespace eular

#endif // __UTP_LOG_H__
