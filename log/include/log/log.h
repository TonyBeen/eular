#ifndef _LOG_H_
#define _LOG_H_

#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <log/level.h>

#ifdef _MSC_VER
    #include <sal.h>
    #define PRINTF_FMT _Printf_format_string_
    #define FORMAT_ATTR(...)
#else
    #define PRINTF_FMT
    #define FORMAT_ATTR(...) __attribute__((format(__VA_ARGS__)))
#endif


#ifndef LOGD
#define LOGD(...) ((void)log_write(LEVEL_DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGI
#define LOGI(...) ((void)log_write(LEVEL_INFO, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGW
#define LOGW(...) ((void)log_write(LEVEL_WARN, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGE
#define LOGE(...) ((void)log_write(LEVEL_ERROR, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGF
#define LOGF(...) ((void)log_write(LEVEL_FATAL, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOG_ASSERT
#define LOG_ASSERT(cond, ...) \
    (!(cond) ? ((void)log_write_assert(LEVEL_FATAL, #cond, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif

#ifndef LOG_ASSERT2
#define LOG_ASSERT2(cond)   \
    (!(cond) ? ((void)log_write_assert(LEVEL_FATAL, #cond, LOG_TAG, "")) : (void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param lev 设置最小输出级别
 */
void log_set_level(log_level_t lev);

/**
 * @brief 设置日志输出目录和文件名前缀。会自动追加 .log 后缀。
 *        例如 path="/tmp/app", file_name="server" 时活动文件为 /tmp/app/server.log
 */
void log_set_path(const char *path, const char *file_name);

/**
 * @brief 配置文件输出轮转策略。
 * @param max_file_size 单个活动日志文件的最大字节数。传 0 表示不按大小轮转。
 * @param max_file_count 轮转后保留的归档文件数量。传 0 表示在开启大小轮转时保留全部归档。
 */
void log_set_file_rotation(uint64_t max_file_size, uint32_t max_file_count);

/**
 * @brief 使输出在stdout的日志携带颜色
 * 
 * @param flag 
 */
void log_enable_color(int32_t flag);

/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void log_add_output_node(output_type_t type);

/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void log_del_output_node(output_type_t type);

void log_write(int32_t level, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 3, 4);

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 4, 5);

#ifdef __cplusplus
}
#endif

#endif