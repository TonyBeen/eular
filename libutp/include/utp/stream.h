/*************************************************************************
    > File Name: stream.h
    > Author: eular
    > Brief: libutp 流抽象，提供类似套接字的读写接口与状态管理。
    > Created Time: Tue 13 Jan 2026 05:53:46 PM CST
 ************************************************************************/

#ifndef __UTP_STREAM_H__
#define __UTP_STREAM_H__

#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <utp/platform.h>

namespace eular {
namespace utp {

/**
 * @class Stream
 * @brief 代表 Connection 中的一个逻辑流。提供读写数据、零拷贝视图、优先级设置以及状态回调。
 */
class UTP_API Stream
{
public:
    /**
     * @enum 优先级常量
     */
    enum {
        kPriorityHighest = 0,   ///< 最高优先级
        kPriorityLowest = 7,    ///< 最低优先级
        kPriorityDefault = 4,   ///< 默认优先级
    };

    /**
     * @struct ConstBufferView
     * @brief 只读缓冲区视图，用于零拷贝读取
     */
    struct ConstBufferView {
        const void *data{nullptr};      ///< 数据指针
        size_t      len{0};             ///< 数据长度
    };

    /**
     * @struct MutableBufferView
     * @brief 可写缓冲区视图，用于零拷贝写入
     */
    struct MutableBufferView {
        void*   data{nullptr};          ///< 数据指针
        size_t  len{0};                 ///< 空间长度
    };

    /**
     * @enum State
     * @brief 流状态机定义
     */
    enum State : uint8_t {
        kStateOpen = 0,             ///< 已打开，可收发
        kStateHalfClosedLocal,      ///< 本地已发送 FIN，不可再写
        kStateHalfClosedRemote,     ///< 收到远程 FIN，对端不再写
        kStateClosed,               ///< 已完全关闭
    };

    using OnReadable = std::function<void()>;
    using OnWritable = std::function<void()>;
    using OnClosed   = std::function<void()>;
    using OnReset    = std::function<void(uint16_t)>;

    Stream() = default;
    virtual ~Stream() = default;

    /**
     * @brief 获取流 ID
     * @return 32 位流 ID
     */
    virtual uint32_t    id() const = 0;

    /**
     * @brief 同步写入数据（拷贝模式）
     * @param data 数据源
     * @param len 数据长度
     * @param fin 是否在数据后携带 FIN 标志
     * @return 实际写入长度，或错误码（负数）
     */
    virtual int32_t     write(const void *data, size_t len, bool fin = false) = 0;

    /**
     * @brief 同步读取数据（拷贝模式）
     * @param buffer 目标缓冲区
     * @param capacity 缓冲区容量
     * @return 实际读取长度，或错误码（负数）
     */
    virtual int32_t     read(void *buffer, size_t capacity) = 0;

    /**
     * @brief 获取可写缓冲区视图（零拷贝模式）
     * 获取后需调用 commitWrite() 提交数据。
     * @param views [out] 长度为 2 的视图数组（应对环形缓冲区绕回）
     * @param maxBytes 期望申请的最大字节数
     * @return 实际可用的总字节数
     */
    virtual size_t      acquireWriteBuffer(MutableBufferView views[2], size_t maxBytes) = 0;

    /**
     * @brief 提交已写入零拷贝缓冲区的数据
     * @param bytes 实际写入的字节数
     * @param fin 是否携带 FIN 标志
     * @return 错误码，0 表示成功
     */
    virtual int32_t     commitWrite(size_t bytes, bool fin = false) = 0;

    /**
     * @brief 获取待读取数据视图（零拷贝模式）
     * 读取后需调用 commitReadViews() 释放已读空间。
     * @param views [out] 长度为 2 的视图数组
     * @param maxBytes 期望读取的最大字节数
     * @return 实际可读的总字节数
     */
    virtual size_t      acquireReadViews(ConstBufferView views[2], size_t maxBytes) const = 0;

    /**
     * @brief 提交已读取的字节数，释放缓冲区
     * @param bytes 已处理的字节数
     * @return 错误码，0 表示成功
     */
    virtual int32_t     commitReadViews(size_t bytes) = 0;

    /**
     * @brief 获取当前流状态
     * @return State 枚举值
     */
    virtual State       state() const = 0;

    /**
     * @brief 检查流是否可读（接收缓冲区有数据）
     * @return true 为可读
     */
    virtual bool        readable() const = 0;

    /**
     * @brief 检查流是否可写（发送缓冲区未满）
     * @return true 为可写
     */
    virtual bool        writable() const = 0;

    /**
     * @brief 主动关闭流（发送 FIN）
     */
    virtual void        close() = 0;

    /**
     * @brief 强制重置流（发送 RESET_STREAM 帧）
     * @param errorCode 重置错误码
     * @return 错误码，0 表示成功
     */
    virtual int32_t     reset(uint16_t errorCode) = 0;

    /**
     * @brief 检查是否收到了对端的重置信号
     * @return true 表示已收到对端的 RESET_STREAM
     */
    virtual bool        resetReceived() const = 0;

    /**
     * @brief 设置流优先级（影响流调度器的公平性/严格优先级逻辑）
     * @param priority 优先级值 (0-7)
     * @return 错误码，0 表示成功
     */
    virtual int32_t     setPriority(uint8_t priority) = 0;

    /**
     * @brief 获取流优先级
     * @return 优先级值
     */
    virtual uint8_t     priority() const = 0;

    /**
     * @brief 设置可读回调
     * @param cb 回调函数
     */
    virtual void setOnReadable(const OnReadable &cb) = 0;

    /**
     * @brief 设置可写回调
     * @param cb 回调函数
     */
    virtual void setOnWritable(const OnWritable &cb) = 0;

    /**
     * @brief 设置关闭回调（双向完全关闭）
     * @param cb 回调函数
     */
    virtual void setOnClosed(const OnClosed &cb) = 0;

    /**
     * @brief 设置重置回调（收到对端重置）
     * @param cb 回调函数
     */
    virtual void setOnReset(const OnReset &cb) = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_STREAM_H__
