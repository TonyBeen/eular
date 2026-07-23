/*************************************************************************
    > File Name: connection.h
    > Author: eular
    > Brief: libutp 连接抽象，管理多个流并提供连接级别的统计与控制。
    > Created Time: Tue 13 Jan 2026 05:42:22 PM CST
 ************************************************************************/

#ifndef __UTP_CONNECTION_H__
#define __UTP_CONNECTION_H__

#include <stdint.h>
#include <string>
#include <memory>
#include <functional>
#include <vector>

#include <utp/platform.h>
#include <utp/stream.h>

namespace eular {
namespace utp {

/**
 * @class Connection
 * @brief 代表一个 libutp 连接，是多个 Stream 的容器，负责处理握手后的数据传输、拥塞控制和 MTU 探测。
 */
class UTP_API Connection
{
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
public:
    using Ptr = std::shared_ptr<Connection>;

    /**
     * @enum StreamType
     * @brief 流类型定义
     */
    enum StreamType : uint8_t {
        kStreamTypeBidirectional = 0,   ///< 双向流
        kStreamTypeUnidirectional = 1,  ///< 单向流
        kStreamTypeAll = 0xFF,          ///< 所有类型（用于统计计数）
    };

    /**
     * @struct ConnectionErrorInfo
     * @brief 连接错误信息
     */
    struct ConnectionErrorInfo {
        int32_t     error_code{0};      ///< 错误码
        std::string error_reason;       ///< 错误原因描述
    };

    /**
     * @struct ConnectionCloseInfo
     * @brief 连接关闭信息
     */
    struct ConnectionCloseInfo {
        int32_t     error_code{0};      ///< 错误码
        std::string error_reason;       ///< 错误原因描述
        bool        by_peer{false};     ///< 是否由对端关闭
    };

    using OnIncomingStream      = std::function<void(Stream *)>;
    using OnSessionTokenReady   = std::function<void()>;
    using OnError               = std::function<void(const ConnectionErrorInfo &)>;
    using OnClosed              = std::function<void(const ConnectionCloseInfo &)>;

    /**
     * @struct Description
     * @brief 连接的元数据描述
     */
    struct Description {
        uint32_t        scid;           ///< 本端 CID (Source CID)
        uint32_t        dcid;           ///< 对端 CID (Destination CID)
        std::string     remoteHost;     ///< 对端 IP 地址
        uint16_t        remotePort;     ///< 对端端口
    };

    /**
     * @struct Statistic
     * @brief 连接级别的运行统计数据
     */
    struct Statistic {
        uint32_t        pmtu;           ///< 路径 MTU (Path MTU)，单位：bytes
        uint32_t        rtt;            ///< 平滑 RTT (SRTT)，单位：us
        uint32_t        rttvar;         ///< RTT 抖动 (RTTVAR)，单位：us
        uint32_t        bw_estimate;    ///< 估算带宽 (pacing rate)，单位：bytes/s
        uint64_t        rx_bytes;       ///< 累计接收字节数，单位：bytes
        uint64_t        tx_bytes;       ///< 累计发送字节数（含重传），单位：bytes
        uint64_t        rtx_bytes;      ///< 累计重传字节数，单位：bytes

        // 流调度器指标
        uint64_t        scheduler_select_total;          ///< 总选流次数
        uint64_t        scheduler_select_disabled;       ///< DISABLED 模式选流次数
        uint64_t        scheduler_select_strict;         ///< STRICT 模式选流次数
        uint64_t        scheduler_select_drr;            ///< DRR 模式选流次数
        uint64_t        scheduler_strict_aging_promoted; ///< STRICT 中 aging 提升次数
        uint64_t        scheduler_would_block;           ///< 选中流发送返回 WOULD_BLOCK 次数
        uint64_t        scheduler_empty_rounds;          ///< flush 未选到可发流次数
        uint64_t        scheduler_mode_switches;         ///< 调度模式热切换次数
        uint64_t        scheduler_drr_refills;           ///< DRR deficit 补充次数
        uint64_t        scheduler_drr_consumes;          ///< DRR deficit 消耗次数
    };

    Connection() = default;
    virtual ~Connection() = default;

    /**
     * @brief 设置新流到达回调（对端发起的流）
     * @param cb 回调函数
     */
    virtual void        setOnIncomingStream(const OnIncomingStream &cb) = 0;

    /**
     * @brief 设置会话票据就绪回调（握手完成后，且服务器下发了票据）
     * @param cb 回调函数
     */
    virtual void        setOnSessionTokenReady(const OnSessionTokenReady &cb) = 0;

    /**
     * @brief 设置错误回调
     * @param cb 回调函数
     */
    virtual void        setOnError(const OnError &cb) = 0;

    /**
     * @brief 设置连接关闭回调
     * @param cb 回调函数
     */
    virtual void        setOnClosed(const OnClosed &cb) = 0;

    /**
     * @brief 获取当前连接中的流数量
     * @param streamType 指定流类型
     * @return 流数量
     */
    virtual int32_t     streamCount(StreamType streamType = kStreamTypeAll) const = 0;

    /**
     * @brief 获取当前可创建的流剩余配额
     * @param streamType 指定流类型
     * @return 剩余配额数量
     */
    virtual int32_t     creatableStreamCount(StreamType streamType) const = 0;

    /**
     * @brief 获取连接统计信息
     * @return Statistic 结构体
     */
    virtual Statistic   statistic() const = 0;

    /**
     * @brief 获取连接元数据描述
     * @return Description 结构体
     */
    virtual Description description() const = 0;

    /**
     * @brief 导出序列化后的会话票据 (Session Ticket)
     * 用于非加密 0-RTT 场景。
     * @param outToken [out] 输出缓冲区
     * @return 错误码，0 表示成功
     */
    virtual int32_t     exportSessionToken(std::vector<uint8_t> &outToken) = 0;

    /**
     * @brief 导出序列化后的会话恢复状态 (Resumption State)
     * 用于加密 0-RTT 场景。
     * @param outState [out] 输出字符串
     * @return 错误码，0 表示成功
     */
    virtual int32_t     exportSessionResumptionState(std::string &outState) = 0;

    /**
     * @brief 创建一个新的本地发起的流
     * @param streamType 指定流类型
     * @return 错误码或流 ID（取决于具体实现，通常 >0 为 ID，<=0 为错误）
     */
    virtual int32_t     createStream(StreamType streamType = kStreamTypeBidirectional) = 0;

    /**
     * @brief 根据 ID 获取流对象
     * @param streamId 流 ID
     * @return 流指针，若不存在则返回 nullptr
     */
    virtual Stream*     getStream(uint32_t streamId) = 0;

    /**
     * @brief 主动关闭连接，发送 CONNECTION_CLOSE 帧
     */
    virtual void        close() = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONNECTION_H__
