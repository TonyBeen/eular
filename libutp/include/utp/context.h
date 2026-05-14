/*************************************************************************
    > File Name: context.h
    > Author: eular
    > Brief: libutp 核心入口，提供上下文管理、连接发起与接受功能。
    > Created Time: Tue 23 Dec 2025 05:14:55 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_H__
#define __UTP_CONTEXT_H__

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <array>

#include <utp/config.h>
#include <utp/platform.h>
#include <utp/connection.h>

struct event;
struct event_base;

namespace eular {
namespace utp {

class ContextImpl;

/**
 * @class Context
 * @brief libutp 的核心上下文类，负责管理所有连接的生命周期、底层 UDP 套接字以及事件循环调度。
 */
class UTP_API Context {
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

public:
    /**
     * @enum EncryptionMode
     * @brief 加密模式定义
     */
    enum EncryptionMode : uint8_t {
        kEncryptionNone = 0,    ///< 不加密
        kEncryptionAesGcm128,   ///< AES-GCM-128 加密
        kEncryptionAesGcm256,   ///< AES-GCM-256 加密
    };

    /**
     * @enum ConnectAttemptType
     * @brief 建连尝试类型，用于区分握手阶段的来源
     */
    enum ConnectAttemptType : uint8_t {
        kConnectAttemptNormal = 0,      ///< 普通握手
        kConnectAttemptZeroRttToken,    ///< 基于票据的 0-RTT 握手
        kConnectAttemptZeroRttState,    ///< 基于状态的 0-RTT 握手
        kConnectAttemptPassive,         ///< 被动接受的握手
    };

    /**
     * @struct Statistic
     * @brief 上下文级别的运行统计信息
     */
    struct Statistic {
        uint64_t zero_rtt_offered{0};                   ///< 尝试 0-RTT 的次数
        uint64_t zero_rtt_accepted{0};                  ///< 0-RTT 被接受的次数
        uint64_t zero_rtt_rejected{0};                  ///< 0-RTT 被拒绝的次数
        uint64_t zero_rtt_replay_rejected{0};           ///< 因重放攻击防御被拒绝的次数
        uint64_t zero_rtt_invalid_ticket_rejected{0};   ///< 因票据无效被拒绝的次数
        uint64_t path_validation_started{0};            ///< 路径验证启动次数
        uint64_t path_validation_succeeded{0};          ///< 路径验证成功次数
        uint64_t path_validation_failed{0};             ///< 路径验证失败次数
    };

    /**
     * @struct ConnectInfo
     * @brief 普通建连配置参数
     */
    struct ConnectInfo {
        std::string ip;                         ///< 目标 IP 地址
        uint16_t    port{0};                    ///< 目标端口
        uint32_t    timeout{3000};              ///< 握手超时时间 (ms)
        int8_t      retries{0};                 ///< 重试次数
        EncryptionMode encrypted{kEncryptionNone}; ///< 加密模式
    };

    /**
     * @struct Connect0RttInfo
     * @brief 基于票据的 0-RTT 建连配置参数
     */
    struct Connect0RttInfo {
        std::string ip;                         ///< 目标 IP 地址
        uint16_t    port{0};                    ///< 目标端口
        uint32_t    timeout{3000};              ///< 握手超时时间 (ms)
        int8_t      retries{0};                 ///< 重试次数
        std::vector<uint8_t> session_ticket;    ///< 会话票据 (Session Ticket)
        std::vector<uint8_t> early_data;        ///< 早数据 (Early Data)
        bool        early_fin{false};           ///< 是否在早数据中携带 FIN 标志
    };

    /**
     * @struct Connect0RttWithStateInfo
     * @brief 基于状态的 0-RTT 建连配置参数（用于加密恢复）
     */
    struct Connect0RttWithStateInfo {
        std::string ip;                         ///< 目标 IP 地址
        uint16_t    port{0};                    ///< 目标端口
        uint32_t    timeout{3000};              ///< 握手超时时间 (ms)
        int8_t      retries{0};                 ///< 重试次数
        std::vector<uint8_t> early_data;        ///< 早数据 (Early Data)
        bool        early_fin{false};           ///< 是否在早数据中携带 FIN 标志
    };

    /**
     * @struct NewConnectionInfo
     * @brief 新连接请求信息，用于服务端拦截
     */
    struct NewConnectionInfo {
        std::string remote_ip;                  ///< 远程 IP
        uint16_t    remote_port{0};             ///< 远程端口
        uint32_t    local_cid{0};               ///< 本端 CID
        uint32_t    peer_cid{0};                ///< 对端 CID
        EncryptionMode encrypted{kEncryptionNone}; ///< 是否要求加密
    };

    /**
     * @struct ZeroRttDecisionInfo
     * @brief 0-RTT 决策结果信息
     */
    struct ZeroRttDecisionInfo {
        std::string remote_ip;                  ///< 远程 IP
        uint16_t    remote_port{0};             ///< 远程端口
        uint32_t    local_cid{0};               ///< 本端 CID
        uint32_t    peer_cid{0};                ///< 对端 CID
        bool        accepted{false};            ///< 是否接受 0-RTT
        std::string reason;                     ///< 拒绝理由（可选）
    };

    /**
     * @struct ConnectAttemptInfo
     * @brief 建连尝试的上下文信息，用于回调透传
     */
    struct ConnectAttemptInfo {
        std::string ip;                         ///< 目标 IP
        uint16_t    port{0};                    ///< 目标端口
        uint32_t    timeout{3000};              ///< 超时时间
        int8_t      retries{0};                 ///< 重试次数
        EncryptionMode encrypted{kEncryptionNone}; ///< 加密模式
        ConnectAttemptType type{kConnectAttemptNormal}; ///< 尝试类型
        uint32_t    session_token_size{0};      ///< 票据大小
        uint32_t    resumption_state_size{0};   ///< 恢复状态大小
        uint32_t    early_data_size{0};         ///< 早数据大小
        bool        early_fin{false};           ///< 是否携带早 FIN
    };

    using OnConnected = std::function<void(Connection::Ptr)>;
    using OnConnectError = std::function<void(int32_t, const std::string &, ConnectAttemptInfo)>;
    using OnConnectionClosed = std::function<void(Connection::Ptr)>;
    using OnNewConnection = std::function<bool(const NewConnectionInfo &)>;
    using OnZeroRttDecision = std::function<void(const ZeroRttDecisionInfo &)>;

    /**
     * @brief 构造函数
     * @param base libevent 事件循环句柄
     * @param config 配置参数，若为 nullptr 则使用默认配置
     */
    Context(event_base *base, Config *config = nullptr);
    ~Context();

public:
    /**
     * @brief 获取库版本号
     * @return 版本号字符串
     */
    static const char *Version();

    /**
     * @brief 设置连接成功回调（主动 connect 时）
     * @param cb 回调函数
     */
    void setOnConnected(const OnConnected &cb);

    /**
     * @brief 设置建连失败回调
     * 触发场景包括：同步校验失败、异步握手超时、CID 冲突、被动拒绝等。
     * @param cb 回调函数
     */
    void setOnConnectError(const OnConnectError &cb);

    /**
     * @brief 设置新连接请求回调（服务端）
     * 对普通被动建连，可在该回调内直接调用 accept() 接受当前连接；
     * 若返回 true 但未立即 accept，则连接会保留在 pending 队列中。
     * @param cb 回调函数，返回 true 表示允许连接，false 表示拒绝
     */
    void setOnNewConnection(const OnNewConnection &cb);

    /**
     * @brief 设置连接关闭回调
     * @param cb 回调函数
     */
    void setOnConnectionClosed(const OnConnectionClosed &cb);

    /**
     * @brief 设置 0-RTT 决策回调（如无效票据、重放拒绝等事件）
     * @param cb 回调函数
     */
    void setOnZeroRttDecision(const OnZeroRttDecision &cb);

    /**
     * @brief 设置会话恢复串封装密钥（32 字节）
     * 未设置时使用 SDK 内置默认密钥。
     * @param secret 密钥向量
     */
    void setResumptionSecret(const std::vector<uint8_t> &secret);

    /**
     * @brief 清除自定义会话恢复密钥，恢复为默认密钥
     */
    void clearResumptionSecret();

    /**
     * @brief 使用会话恢复状态发起 0-RTT 建连（支持加密场景）
     * @param info 建连参数
     * @param state 序列化后的恢复状态串
     * @return 错误码，0 表示成功
     */
    int32_t connect0RttWithState(const Connect0RttWithStateInfo &info, const std::string &state);

    /**
     * @brief 获取当前上下文的统计信息
     * @return Statistic 结构体
     */
    Statistic statistic() const;

    /**
     * @brief 绑定本地 UDP 端口
     * @param ip 监听 IP
     * @param port 监听端口
     * @param ifname 绑定的网卡接口名（可选）
     * @return 错误码，0 表示成功
     */
    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname = "");

    /**
     * @brief 发起普通握手建连
     * @param info 建连参数
     * @return 错误码，0 表示成功
     */
    int32_t connect(const ConnectInfo &info);

    /**
     * @brief 发起票据式 0-RTT 建连（仅支持非加密场景）
     * @param info 建连参数
     * @return 错误码，0 表示成功
     */
    int32_t connect0Rtt(const Connect0RttInfo &info);

    /**
     * @brief 接受处于 pending 状态的被动连接
     * @return 错误码，0 表示成功
     */
    int32_t accept();

private:
    std::shared_ptr<ContextImpl>    m_impl{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_H__
