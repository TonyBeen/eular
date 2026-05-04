/*************************************************************************
    > File Name: transport_params.h
    > Author: eular
    > Brief:
    > Created Time: Tue 01 Apr 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_TRANSPORT_PARAMS_H__
#define __UTP_PROTO_FRAME_TRANSPORT_PARAMS_H__

#include "proto/frame.h"
#include "util/transport_param.h"

#define FRAME_TRANSPORT_PARAMS_SIZE (1 + 2 + 4 + 2 + 2 + 2 + 1 + 8 + 8 + 8)

namespace eular {
namespace utp {

struct FrameTransportParams : public FrameBase {
public:
    FrameTransportParams() : FrameBase(FrameType::kFrameTransportParams) {}
    ~FrameTransportParams() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    TransportParams *params{nullptr};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_TRANSPORT_PARAMS_H__