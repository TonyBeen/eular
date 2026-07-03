/*************************************************************************
    > File Name: network_path.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Feb 2026 10:59:11 AM CST
 ************************************************************************/

#include "util/network_path.h"

#include "utp/errno.h"
#include "util/random.hpp"

namespace eular {
namespace utp {

NetworkPath::NetworkPath(uint32_t challengeTimeoutMs, uint8_t maxChallengeRetries) :
    m_challengeTimeoutMs(challengeTimeoutMs > 0 ? challengeTimeoutMs : 1500),
    m_maxChallengeRetries(maxChallengeRetries > 0 ? maxChallengeRetries : 3)
{
}

void NetworkPath::bindPeerAddress(const Address &address)
{
    m_peerAddress = address;
    m_retryCount = 0;
    m_hasPendingChallenge = false;
    m_challengeDeadlineMs = 0;
    m_state = address.isValid() ? kPathValidated : kPathUnknown;
}

bool NetworkPath::detectPeerAddressChange(const Address &newAddress)
{
    if (!newAddress.isValid()) {
        return false;
    }

    if (!m_peerAddress.isValid()) {
        bindPeerAddress(newAddress);
        return false;
    }

    if (m_peerAddress == newAddress) {
        return false;
    }

    m_peerAddress = newAddress;
    m_state = kPathValidating;
    m_retryCount = 0;
    m_hasPendingChallenge = false;
    m_challengeDeadlineMs = 0;
    return true;
}

bool NetworkPath::needPathValidation() const
{
    return m_state == kPathValidating;
}

int32_t NetworkPath::makePathChallenge(FramePathChallenge &frame, utp_time_t nowMs)
{
    if (m_state != kPathValidating) {
        return UTP_ERR_INVALID_STATE;
    }

    if (m_hasPendingChallenge && nowMs < m_challengeDeadlineMs) {
        return UTP_ERR_IN_PROGRESS;
    }

    if (!canRetryChallenge()) {
        m_state = kPathFailed;
        m_hasPendingChallenge = false;
        return UTP_ERR_TIMEOUT;
    }

    frame.type = FrameType::kFramePathChallenge;
    RandomBytes(frame.data.data(), frame.data.size());
    m_pendingChallenge = frame.data;
    m_hasPendingChallenge = true;
    m_challengeDeadlineMs = nowMs + m_challengeTimeoutMs;
    ++m_retryCount;
    return UTP_ERR_OK;
}

void NetworkPath::makePathResponse(const FramePathChallenge &challenge, FramePathResponse &response) const
{
    response.type = FrameType::kFramePathResponse;
    response.data = challenge.data;
}

bool NetworkPath::onPathResponse(const FramePathResponse &response)
{
    if (!m_hasPendingChallenge || m_state != kPathValidating) {
        return false;
    }

    if (!IsChallengeEqual(m_pendingChallenge, response.data)) {
        return false;
    }

    m_state = kPathValidated;
    m_retryCount = 0;
    m_hasPendingChallenge = false;
    m_challengeDeadlineMs = 0;
    return true;
}

bool NetworkPath::onTimeout(utp_time_t nowMs)
{
    if (!m_hasPendingChallenge || m_state != kPathValidating) {
        return false;
    }

    if (nowMs < m_challengeDeadlineMs) {
        return false;
    }

    m_hasPendingChallenge = false;
    if (!canRetryChallenge()) {
        m_state = kPathFailed;
        return true;
    }

    return false;
}

bool NetworkPath::canRetryChallenge() const
{
    return m_retryCount < m_maxChallengeRetries;
}

bool NetworkPath::IsChallengeEqual(const std::array<uint8_t, FRAME_PATH_DATA_SIZE> &lhs,
                                   const std::array<uint8_t, FRAME_PATH_DATA_SIZE> &rhs)
{
    return lhs == rhs;
}

} // namespace utp
} // namespace eular
