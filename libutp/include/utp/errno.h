/*************************************************************************
    > File Name: errno.h
    > Author: hsz
    > Brief:
    > Created Time: Fri 26 Dec 2025 04:14:22 PM CST
 ************************************************************************/

#ifndef __UTP_ERRNO_H__
#define __UTP_ERRNO_H__

#include <stdint.h>

typedef enum {
    /* 通用错误 */
    UTP_ERR_NO_ERROR             = 0x0000,  // 无错误，正常关闭
    UTP_ERR_INTERNAL_ERROR       = 0x0001,  // 内部错误
    UTP_ERR_CANCELLED            = 0x0002,  // 应用层取消
    UTP_ERR_TIMEOUT              = 0x0003,  // 超时
    UTP_ERR_VERSION_MISMATCH     = 0x0004,  // 协议版本不匹配

    // stream
    UTP_ERR_STREAM_CLOSED        = 0x0010,  // 流已关闭
    UTP_ERR_STREAM_NOT_FOUND     = 0x0011,  // 流不存在
    UTP_ERR_STREAM_STATE_ERROR   = 0x0012,  // 流状态错误
    UTP_ERR_STREAM_LIMIT_ERROR   = 0x0013,  // 超过流数量限制
    UTP_ERR_STREAM_FLOW_CONTROL  = 0x0020,  // 流量控制违规
    UTP_ERR_STREAM_DATA_BLOCKED  = 0x0021,  // 流数据阻塞
    UTP_ERR_STREAM_DATA_LIMITED  = 0x0022,  // 超过数据限制

    /* 帧错误 */
    UTP_ERR_FRAME_FORMAT_ERROR   = 0x0030,  // 帧格式错误
    UTP_ERR_FRAME_UNEXPECTED     = 0x0031,  // 意外的帧类型

    /* 应用层错误 (0x0100 - 0xFFFF 由应用自定义) */
    UTP_ERR_APP_ERROR_BASE       = 0x0100,  // 应用层错误起始值
} utp_error_t;

#endif // __UTP_ERRNO_H__
