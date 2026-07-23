/*************************************************************************
    > File Name: traffic_key_schedule.h
    > Brief: Direction-separated traffic key derivation for a UTP session.
 ************************************************************************/

#ifndef __UTP_CRYPTO_TRAFFIC_KEY_SCHEDULE_H__
#define __UTP_CRYPTO_TRAFFIC_KEY_SCHEDULE_H__

#include <array>
#include <cstddef>
#include <cstdint>

#include "crypto/x25519_wrapper.h"
#include "crypto/aes_gcm_context.h"
#include "util/status.h"

namespace eular {
namespace utp {

class TrafficKeySchedule
{
public:
    static constexpr size_t MAX_KEY_SIZE = 32;
    static constexpr size_t NONCE_PREFIX_SIZE = 4;

    struct Secret {
        std::array<uint8_t, MAX_KEY_SIZE> key{};
        std::array<uint8_t, NONCE_PREFIX_SIZE> noncePrefix{};
    };

    struct Material {
        Secret clientToServer;
        Secret serverToClient;
        size_t keySize{0};
    };

    static Status Derive(const X25519Wrapper::SharedSecret &sharedSecret,
                         const X25519Wrapper::PublicKey &clientPublicKey,
                         const X25519Wrapper::PublicKey &serverPublicKey,
                         uint32_t clientCid,
                         uint32_t serverCid,
                         uint8_t cryptoType,
                         size_t keySize,
                         Material &out);

    static Status CreateAesGcmContexts(const X25519Wrapper &x25519,
                                       const X25519Wrapper::PublicKey &peerPublicKey,
                                       uint32_t localCid,
                                       uint32_t peerCid,
                                       bool localIsClient,
                                       uint8_t cryptoType,
                                       size_t keySize,
                                       std::shared_ptr<AesGcmContext> &tx,
                                       std::shared_ptr<AesGcmContext> &rx);
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_TRAFFIC_KEY_SCHEDULE_H__
