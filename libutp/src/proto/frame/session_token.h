/*************************************************************************
    > File Name: session_token.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:47 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_SESSION_TOKEN_H__
#define __UTP_PROTO_FRAME_SESSION_TOKEN_H__

#include <vector>

#include "proto/frame.h"

#define FRAME_SESSION_TOKEN_HDR_SIZE   (1 + 1 + 2) // type + token_size + token_validity_period

namespace eular {
namespace utp {

struct FrameSessionToken : public FrameBase {
public:
    FrameSessionToken() : FrameBase(FrameType::kFrameSessionToken) {}
    ~FrameSessionToken() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint8_t                 token_size{0};
    uint16_t                token_validity_period{0}; // seconds
    std::vector<uint8_t>    token;
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_SESSION_TOKEN_H__
