/*************************************************************************
    > File Name: logger.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:58:58 PM CST
 ************************************************************************/

#ifndef __UTP_LOGGER_H__
#define __UTP_LOGGER_H__

#include <stdint.h>

#include <utp/platform.h>

typedef enum {
    UTP_LOG_DEBUG,
    UTP_LOG_INFO,
    UTP_LOG_WARN,
    UTP_LOG_ERROR,
    UTP_LOG_FATAL,
    UTP_LOG_SILENT,
} utp_log_level_t;

typedef void (*utp_log_callback_t) (int32_t /* level */, const char * /* log_message */, int32_t /* message_size */);

EXTERN_C_BEGIN

// thread unsafe
UTP_API void utp_set_log_cb(utp_log_callback_t log_cb);

// thread unsafe, default UTP_LOG_SILENT
UTP_API void utp_set_log_level(utp_log_level_t level);

EXTERN_C_END

#endif // __UTP_LOGGER_H__
