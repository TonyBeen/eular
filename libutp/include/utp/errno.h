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

typedef enum : uint16_t {
    /* 通用错误 */
    UTP_ERR_OK                  = 0x0000,   // 无错误，正常关闭
    UTP_ERR_INVALID_PARAM,                  // 无效参数
    UTP_ERR_INTERNAL_ERROR,                 // 内部错误
    UTP_ERR_CANCELLED,                      // 应用层取消
    UTP_ERR_TIMEOUT,                        // 超时
    UTP_ERR_VERSION_MISMATCH,               // 协议版本不匹配
    UTP_ERR_NOT_IMPLEMENTED,                // 功能未实现
    UTP_ERR_BUSY,                           // 资源忙
    UTP_ERR_NO_MEMORY,                      // 内存不足
    UTP_ERR_IN_PROGRESS,                    // 操作正在进行中
    UTP_ERR_WOULD_BLOCK,                    // 操作会阻塞
    UTP_ERR_INVALID_STATE,                  // 状态无效
    UTP_ERR_CONNECTION_CLOSING,             // 连接正在关闭，数据面发送被禁止
    UTP_ERR_CID_CONFLICT,                   // CID冲突
    UTP_ERR_OVERFLOW,                       // 溢出
    UTP_ERR_STREAM_LIMITED,                 // 流个数被限制(连接级, 触发ConnectionClose)

    // socket
    UTP_ERR_SOCKET_CREATE       = 0x0020,   // Socket错误
    UTP_ERR_SOCKET_OPTION,                  // Socket选项错误
    UTP_ERR_SOCKET_NOT_BOUND,               // Socket未绑定
    UTP_ERR_SOCKET_BIND,                    // Socket绑定IP错误
    UTP_ERR_SOCKET_IOCTL,                   // IO控制错误
    UTP_ERR_SOCKET_READ,                    // 读取Socket错误
    UTP_ERR_SOCKET_WRITE,                   // 写入Socket错误
    UTP_ERR_SOCKET_CONNECTED,               // 已经连接

    // stream
    UTP_ERR_STREAM_CLOSED        = 0x0040,  // 流已关闭
    UTP_ERR_STREAM_NOT_FOUND,               // 流不存在
    UTP_ERR_STREAM_STATE_ERROR,             // 流状态错误
    UTP_ERR_STREAM_LIMIT_ERROR,             // 超过流数量限制
    UTP_ERR_STREAM_FLOW_CONTROL,            // 流量控制违规
    UTP_ERR_STREAM_DATA_BLOCKED,            // 流数据阻塞
    UTP_ERR_STREAM_DATA_LIMITED,            // 超过数据限制
    UTP_ERR_STREAM_ID_EXHAUSTED,            // 流ID耗尽

    /* 帧错误 */
    UTP_ERR_FRAME_FORMAT_ERROR   = 0x0060,  // 帧格式错误
    UTP_ERR_FRAME_UNEXPECTED,               // 意外的帧类型

    // 加密
    UTP_ERR_CRYPTO_UNINITIALIZED = 0x0080,  // 加密模块未初始化
    UTP_ERR_CRYPTO_INIT_FAILED,             // 加密模块初始化失败
    UTP_ERR_CRYPTO_ENCRYPTION,              // 加密错误
    UTP_ERR_CRYPTO_DECRYPTION,              // 解密错误
    UTP_ERR_RANDOM_GENERATION_FAILED,       // 随机数生成失败

    /* 应用层错误 (0x0100 - 0xFFFF 由应用自定义) */
    UTP_ERR_APP_ERROR_BASE       = 0x0100,  // 应用层错误起始值
} utp_error_t;

UTP_API int32_t GetLastError();
UTP_API const char* GetErrorString();

#endif // __UTP_ERRNO_H__
