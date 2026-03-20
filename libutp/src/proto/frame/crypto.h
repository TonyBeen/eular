/*************************************************************************
    > File Name: crypto.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:36 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_CRYPTO_H__
#define __UTP_PROTO_FRAME_CRYPTO_H__

#include <cstddef>

#include "proto/frame.h"
#include "util/transport_param.h"

#define FRAME_CRYPTO_EPH_PUBKEY_SIZE    (32)
#define FRAME_CRYPTO_TP_SIZE            (2 + 4 + 2 + 2 + 2 + 1 + 2 + 2)
#define FRAME_CRYPTO_HDR_SIZE           (1 + 1 + 1)
#define FRAME_CRYPTO_SIZE               (FRAME_CRYPTO_HDR_SIZE + FRAME_CRYPTO_TP_SIZE + FRAME_CRYPTO_EPH_PUBKEY_SIZE)

namespace eular {
namespace utp {
struct FrameCrypto : public FrameBase {
public:
    FrameCrypto() : FrameBase(FrameType::kFrameCrypto) {}
    ~FrameCrypto() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    FrameCryptoType     crypto_type;    // 加密算法类型
    uint8_t             tp_size;        // tp参数个数
    TransportParams*    tp;             // tp参数列表, 长度为tp_size
    void*               eph_pubkey;     // x25519 公钥
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_CRYPTO_H__
