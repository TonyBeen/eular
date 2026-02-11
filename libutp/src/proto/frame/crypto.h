/*************************************************************************
    > File Name: crypto.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:36 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_CRYPTO_H__
#define __UTP_PROTO_FRAME_CRYPTO_H__

#include "proto/frame.h"
#include "util/transport_param.h"

namespace eular {
namespace utp {
struct FrameCrypto : public FrameBase {
public:
    FrameCrypto() : FrameBase(FrameType::kFrameCrypto) {}
    ~FrameCrypto() = default;

public:
    FrameCryptoType     crypto_type;    // 加密算法类型
    uint8_t             tp_size;        // tp参数个数
    TransportParams*    tp;             // tp参数列表, 长度为tp_size
    void*               eph_pubkey;     // x25519 公钥
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_CRYPTO_H__
