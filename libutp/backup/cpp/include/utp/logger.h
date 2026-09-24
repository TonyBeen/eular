/*************************************************************************
    > File Name: logger.h
    > Author: eular
    > Brief: libutp 日志接口定义。
    > Created Time: Mon 08 Dec 2025 04:58:58 PM CST
 ************************************************************************/

#ifndef __UTP_LOGGER_H__
#define __UTP_LOGGER_H__

#include <stdint.h>

#include <utp/platform.h>

/**
 * @enum utp_log_level_t
 * @brief 日志级别定义
 */
typedef enum {
    UTP_LOG_DEBUG,  ///< 调试信息
    UTP_LOG_INFO,   ///< 关键行为信息
    UTP_LOG_WARN,   ///< 警告（可恢复的异常）
    UTP_LOG_ERROR,  ///< 错误（可能导致连接失败）
    UTP_LOG_FATAL,  ///< 致命错误（导致 Context 无法运行）
    UTP_LOG_SILENT, ///< 静默模式（不输出任何日志）
} utp_log_level_t;

/**
 * @typedef utp_log_callback_t
 * @brief 日志回调函数原型
 * @param level 日志级别 (utp_log_level_t)
 * @param log_message 日志文本
 * @param message_size 文本长度
 */
typedef void (*utp_log_callback_t) (int32_t /* level */, const char * /* log_message */, int32_t /* message_size */);

EXTERN_C_BEGIN

/**
 * @brief 设置自定义日志回调函数
 * 注意：该函数非线程安全，建议在 Context 创建前设置。
 * @param log_cb 回调函数指针
 */
UTP_API void utp_set_log_cb(utp_log_callback_t log_cb);

/**
 * @brief 设置日志输出级别
 * 注意：该函数非线程安全，默认级别为 UTP_LOG_SILENT。
 * @param level 目标日志级别
 */
UTP_API void utp_set_log_level(utp_log_level_t level);

EXTERN_C_END

#endif // __UTP_LOGGER_H__
