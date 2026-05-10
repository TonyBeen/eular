/*************************************************************************
    > File Name: errno.h
    > Author: eular
    > Brief: libutp 错误码定义。
    > Created Time: Fri 26 Dec 2025 04:14:22 PM CST
 ************************************************************************/

#ifndef __UTP_ERRNO_H__
#define __UTP_ERRNO_H__

#include <stdint.h>

#include <utp/platform.h>

/**
 * @enum utp_error_codes
 * @brief libutp 错误码枚举
 */
enum {
    // --- 通用错误 ---
    UTP_ERR_OK                  = 0x0000,   ///< 无错误，正常完成
    UTP_ERR_INVALID_PARAM,                  ///< 无效参数
    UTP_ERR_INTERNAL_ERROR,                 ///< 内部逻辑错误
    UTP_ERR_CANCELLED,                      ///< 操作被应用层显式取消
    UTP_ERR_TIMEOUT,                        ///< 操作超时（如握手超时、保活超时）
    UTP_ERR_VERSION_MISMATCH,               ///< 协议版本不匹配
    UTP_ERR_NOT_IMPLEMENTED,                ///< 所请求的功能尚未实现
    UTP_ERR_BUSY,                           ///< 资源忙
    UTP_ERR_NO_MEMORY,                      ///< 内存分配失败
    UTP_ERR_IN_PROGRESS,                    ///< 操作正在异步进行中
    UTP_ERR_WOULD_BLOCK,                    ///< 操作会阻塞（非阻塞模式下的典型返回）
    UTP_ERR_INVALID_STATE,                  ///< 当前对象状态下不支持该操作
    UTP_ERR_CONNECTION_CLOSING,             ///< 连接正在关闭，数据面发送已被禁止
    UTP_ERR_CID_CONFLICT,                   ///< 连接 ID 冲突
    UTP_ERR_OVERFLOW,                       ///< 缓冲区或数值溢出
    UTP_ERR_STREAM_LIMITED,                 ///< 流个数被对端限制（触发连接关闭）
    UTP_ERR_PATH_VALIDATION_BLOCKED = 0x0010, ///< 路径校验阶段受 anti-amplification 限制，发送被抑制
    UTP_ERR_SESSION_TOKEN_UNAVAILABLE,        ///< 当前连接无可导出的会话票据
    UTP_ERR_RESUMPTION_STATE_UNAVAILABLE,     ///< 当前连接无可导出的恢复状态
    UTP_ERR_CONTEXT_UNAVAILABLE,              ///< 连接上下文不可用

    // --- Socket 相关错误 (0x0020 起) ---
    UTP_ERR_SOCKET_CREATE       = 0x0020,   ///< 套接字创建失败
    UTP_ERR_SOCKET_OPTION,                  ///< 设置套接字选项失败
    UTP_ERR_SOCKET_NOT_BOUND,               ///< 套接字未绑定
    UTP_ERR_SOCKET_BIND,                    ///< 绑定 IP/端口失败
    UTP_ERR_SOCKET_IOCTL,                   ///< IO 控制操作失败
    UTP_ERR_SOCKET_READ,                    ///< 从套接字读取失败
    UTP_ERR_SOCKET_WRITE,                   ///< 向套接字写入失败
    UTP_ERR_SOCKET_CONNECTED,               ///< 套接字已处于连接状态
    UTP_ERR_SOCKET_EVENT,                   ///< 事件循环处理失败

    // --- Stream 相关错误 (0x0040 起) ---
    UTP_ERR_STREAM_CLOSED        = 0x0040,  ///< 流已关闭
    UTP_ERR_STREAM_NOT_FOUND,               ///< 流 ID 不存在
    UTP_ERR_STREAM_STATE_ERROR,             ///< 流状态与操作不匹配
    UTP_ERR_STREAM_LIMIT_ERROR,             ///< 超过最大流并发数限制
    UTP_ERR_STREAM_FLOW_CONTROL,            ///< 触发流量控制违规
    UTP_ERR_STREAM_DATA_BLOCKED,            ///< 流数据被阻塞
    UTP_ERR_STREAM_DATA_LIMITED,            ///< 超过单个流的数据总量限制
    UTP_ERR_STREAM_ID_EXHAUSTED,            ///< 流 ID 已用完

    // --- 帧处理错误 (0x0060 起) ---
    UTP_ERR_FRAME_FORMAT_ERROR   = 0x0060,  ///< 帧格式解析错误
    UTP_ERR_FRAME_UNEXPECTED,               ///< 收到不应在当前状态出现的帧类型

    // --- 加密与安全错误 (0x0080 起) ---
    UTP_ERR_CRYPTO_UNINITIALIZED = 0x0080,  ///< 加密模块未初始化
    UTP_ERR_CRYPTO_INIT_FAILED,             ///< 加密模块初始化失败（如密钥长度错误）
    UTP_ERR_CRYPTO_ENCRYPTION,              ///< AEAD 加密操作失败
    UTP_ERR_CRYPTO_DECRYPTION,              ///< AEAD 解密或完整性校验失败
    UTP_ERR_RANDOM_GENERATION_FAILED,       ///< 随机数生成失败

    // --- 应用层错误起始 (0x0100 起) ---
    UTP_ERR_APP_ERROR_BASE       = 0x0100,  ///< 应用层自定义错误起始值 (0x0100 - 0xFFFF)
};

/**
 * @typedef utp_error_t
 * @brief libutp 错误码类型定义
 */
typedef uint32_t utp_error_t;

/**
 * @brief 获取当前线程最后一次发生的错误码
 * @return 错误码值
 */
UTP_API int32_t     utp_get_last_error();

/**
 * @brief 获取最后一次错误对应的描述字符串
 * @return 错误描述字符串指针
 */
UTP_API const char* utp_get_error_string();

#endif // __UTP_ERRNO_H__
