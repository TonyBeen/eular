/*************************************************************************
    > File Name: frame.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 20 Jan 2026 02:40:02 PM CST
 ************************************************************************/

#include "proto/frame.h"
#include "frame.h"

namespace eular {
namespace utp {

std::string FrameTypeToString(uint32_t type)
{
    static const char *frame2string[kFrameMax] = {
        "Invalid",
        "Stream",
        "Ack",
        "Padding",
        "ConnectionClose",
        "Ping",
        "ResetStream",
        "StreamsBlocked",
        "MaxStreams",
        "PathChallenge",
        "PathResponse",
        "Crypto",
        "SessionToken",
        "AckFrequency",
        "Version",
        "HandshakeDone",
        "TransportParams",
        "HandshakeDelay",
    };

    if (type == kFrameInvalid) {
        return "Invalid";
    }

    char buffer[32];
    std::string result;
    for (auto i = 0; i < kFrameMax; ++i) {
        if (type & (1 << i)) {
            if (!result.empty()) {
                result += "|";
            }

            if (frame2string[i]) {
                result += frame2string[i];
            } else {
                snprintf(buffer, sizeof(buffer), "UnknownFrame(%u)", i);
                result += buffer;
            }
        }
    }

    return result;
}

} // namespace utp
} // namespace eular
