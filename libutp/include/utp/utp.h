/*************************************************************************
    > File Name: utp.h
    > Author: eular
    > Brief:
    > Created Time: Tue 23 Dec 2025 05:14:55 PM CST
 ************************************************************************/

#ifndef __UTP_UTP_H__
#define __UTP_UTP_H__

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
class UTP_API Context {
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

public:
    enum EncryptionMode : uint8_t {
        kEncryptionNone = 0,
        kEncryptionAesGcm128,
        kEncryptionAesGcm256,
    };

    struct Statistic {
        uint64_t zero_rtt_offered{0};
        uint64_t zero_rtt_accepted{0};
        uint64_t zero_rtt_rejected{0};
        uint64_t zero_rtt_replay_rejected{0};
    };

    struct ConnectInfo {
        std::string ip;
        uint16_t    port{0};
        uint32_t    timeout{3000}; // ms
        int8_t      retries{0};
        EncryptionMode encrypted{kEncryptionNone}; // None / AES-GCM-128 / AES-GCM-256
        bool        enable_0rtt{false};
        std::vector<uint8_t> session_ticket;
        std::vector<uint8_t> resumption_psk; // 0-RTT resume PSK, recommend 32 bytes
        std::vector<uint8_t> early_data;
        bool        early_fin{false};
    };

    using Connect0RttInfo = ConnectInfo;

    struct Connect0RttWithStateInfo {
        std::string ip;
        uint16_t    port{0};
        uint32_t    timeout{3000}; // ms
        int8_t      retries{0};
        std::vector<uint8_t> early_data;
        bool        early_fin{false};
    };

    struct NewConnectionInfo {
        std::string remote_ip;
        uint16_t    remote_port{0};
        uint32_t    local_cid{0};
        uint32_t    peer_cid{0};
        EncryptionMode encrypted{kEncryptionNone};
    };

    struct ZeroRttDecisionInfo {
        std::string remote_ip;
        uint16_t    remote_port{0};
        uint32_t    local_cid{0};
        uint32_t    peer_cid{0};
        bool        accepted{false};
        std::string reason;
    };

    using OnConnected = std::function<void(Connection::Ptr)>;
    using OnConnectError = std::function<void(int32_t, const std::string &, ConnectInfo)>;
    using OnConnectionClosed = std::function<void(Connection::Ptr)>;
    using OnNewConnection = std::function<bool(const NewConnectionInfo &)>;
    using OnZeroRttDecision = std::function<void(const ZeroRttDecisionInfo &)>;
    using OnSessionResumptionReady = std::function<void()>;

    Context(event_base *base, Config *config = nullptr);
    ~Context();

public:
    static const char *Version();

    // 主动调用connect接口时回调
    void setOnConnected(const OnConnected &cb);
    // 主动调用connect接口失败时回调
    void setOnConnectError(const OnConnectError &cb);
    // 被动接受连接时回调，返回true表示放行, false表示拒绝。
    // 对普通被动建连，可在该回调内直接调用 accept() 接受当前连接；
    // 若返回true但未立即accept，则连接会保留在pending队列中，后续可再调用accept()。
    void setOnNewConnection(const OnNewConnection &cb);
    // 连接关闭时回调
    void setOnConnectionClosed(const OnConnectionClosed &cb);
    // 0-RTT 接受/拒绝事件（如无效票据、重放拒绝）
    void setOnZeroRttDecision(const OnZeroRttDecision &cb);
    // 收到可导出的会话恢复信息后通知应用，应用可主动调用 exportSessionResumptionState()
    void setOnSessionResumptionReady(const OnSessionResumptionReady &cb);

    // 设置会话恢复串封装密钥（32字节），未设置时使用SDK内置默认密钥
    void setResumptionSecret(const std::vector<uint8_t> &secret);
    void clearResumptionSecret();

    // 导出 opaque 会话恢复串（标准Base64），内容由SDK加密封装
    int32_t exportSessionResumptionState(std::string &outState);
    // 使用 opaque 会话恢复串发起 0-RTT
    int32_t connect0RttWithState(const Connect0RttWithStateInfo &info, const std::string &state);

    Statistic statistic() const;

    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname = "");
    int32_t connect(const ConnectInfo &info);
    int32_t connect0Rtt(const Connect0RttInfo &info);
    int32_t accept();

private:
    std::shared_ptr<ContextImpl>    m_impl{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTP_H__
