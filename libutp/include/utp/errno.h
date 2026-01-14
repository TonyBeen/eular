/*************************************************************************
    > File Name: errno.h
    > Author: eular
    > Brief:
    > Created Time: Fri 26 Dec 2025 04:14:22 PM CST
 ************************************************************************/

#ifndef __UTP_ERRNO_H__
#define __UTP_ERRNO_H__

#include <stdint.h>

#include <utp/platform.h>

typedef enum {
    /* 通用错误 */
    UTP_ERR_NO_ERROR             = 0x0000,  // 无错误，正常关闭
    UTP_ERR_INVALID_ARGUMENT,               // 无效参数
    UTP_ERR_INTERNAL_ERROR,                 // 内部错误
    UTP_ERR_CANCELLED,                      // 应用层取消
    UTP_ERR_TIMEOUT,                        // 超时
    UTP_ERR_VERSION_MISMATCH,               // 协议版本不匹配
    UTP_ERR_NOT_IMPLEMENTED,                // 功能未实现
    UTP_ERR_BUSY,                           // 资源忙
    UTP_ERR_NO_MEMORY,                      // 内存不足

    // socket
    UTP_ERR_SOCKET_CREATE       = 0x0010,   // Socket错误
    UTP_ERR_SOCKET_OPTION,                  // Socket选项错误
    UTP_ERR_NOT_BOUND,                      // Socket未绑定
    UTP_ERR_SOCKET_BIND,                    // Socket绑定IP错误
    UTP_ERR_SOCKET_IOCTL,                   // IO控制错误
    UTP_ERR_SOCKET_READ,                    // 读取Socket错误
    UTP_ERR_SOCKET_WRITE,                   // 写入Socket错误

    // stream
    UTP_ERR_STREAM_CLOSED        = 0x0020,  // 流已关闭
    UTP_ERR_STREAM_NOT_FOUND,               // 流不存在
    UTP_ERR_STREAM_STATE_ERROR,             // 流状态错误
    UTP_ERR_STREAM_LIMIT_ERROR,             // 超过流数量限制
    UTP_ERR_STREAM_FLOW_CONTROL,            // 流量控制违规
    UTP_ERR_STREAM_DATA_BLOCKED,            // 流数据阻塞
    UTP_ERR_STREAM_DATA_LIMITED,            // 超过数据限制

    /* 帧错误 */
    UTP_ERR_FRAME_FORMAT_ERROR   = 0x0030,  // 帧格式错误
    UTP_ERR_FRAME_UNEXPECTED,               // 意外的帧类型

    // 加密
    UTP_ERR_CRYPTO_UNINITIALIZED = 0x0040,  // 加密模块未初始化
    UTP_ERR_CRYPTO_INIT_FAILED,             // 加密模块初始化失败
    UTP_ERR_ENCRYPTION_ERROR,               // 加密错误
    UTP_ERR_DECRYPTION_ERROR,               // 解密错误

    /* 应用层错误 (0x0100 - 0xFFFF 由应用自定义) */
    UTP_ERR_APP_ERROR_BASE       = 0x0100,  // 应用层错误起始值
} utp_error_t;

UTP_API int32_t GetLastError();
UTP_API const char* GetErrorString();

#endif // __UTP_ERRNO_H__
