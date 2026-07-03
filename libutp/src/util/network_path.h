/*************************************************************************
    > File Name: network_path.h
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Feb 2026 10:59:09 AM CST
 ************************************************************************/

#ifndef __UTP_UTIL_NETWORK_PATH_H__
#define __UTP_UTIL_NETWORK_PATH_H__

#include <array>

#include "utp/types.h"
#include "socket/address.h"
#include "proto/frame/path.h"

namespace eular {
namespace utp {

class NetworkPath {
public:
    enum State : uint8_t {
        kPathUnknown = 0,
        kPathValidated,
        kPathValidating,
        kPathFailed,
    };

    explicit NetworkPath(uint32_t challengeTimeoutMs = 1500, uint8_t maxChallengeRetries = 3);
    ~NetworkPath() = default;

public:
    void            bindPeerAddress(const Address &address);
    const Address&  peerAddress() const { return m_peerAddress; }

    // 返回 true 表示路径发生变化，进入 validating 状态
    bool detectPeerAddressChange(const Address &newAddress);

    bool needPathValidation() const;
    bool hasInFlightChallenge() const { return m_hasPendingChallenge; }
    State state() const { return m_state; }

    // 生成 challenge 帧，返回 UTP_ERR_OK 表示成功
    int32_t makePathChallenge(FramePathChallenge &frame, utp_time_t nowMs);
    // challenge 的被动响应（原样拷贝 challenge data）
    void    makePathResponse(const FramePathChallenge &challenge, FramePathResponse &response) const;

    // 验证 response，成功后切换到 validated
    bool onPathResponse(const FramePathResponse &response);

    // 检查是否超时；超时且达到重试上限时进入 failed
    bool onTimeout(utp_time_t nowMs);

    // 在重试上限内，是否允许继续发送 challenge
    bool canRetryChallenge() const;

    uint8_t retryCount() const { return m_retryCount; }
    uint8_t maxChallengeRetries() const { return m_maxChallengeRetries; }
    utp_time_t challengeDeadlineMs() const { return m_challengeDeadlineMs; }

private:
    static bool IsChallengeEqual(const std::array<uint8_t, FRAME_PATH_DATA_SIZE> &lhs,
                                 const std::array<uint8_t, FRAME_PATH_DATA_SIZE> &rhs);

private:
    Address     m_peerAddress;
    State       m_state{kPathUnknown};

    std::array<uint8_t, FRAME_PATH_DATA_SIZE> m_pendingChallenge{};
    bool        m_hasPendingChallenge{false};
    utp_time_t  m_challengeDeadlineMs{0};

    uint32_t    m_challengeTimeoutMs{1500};
    uint8_t     m_retryCount{0};
    uint8_t     m_maxChallengeRetries{3};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_NETWORK_PATH_H__
