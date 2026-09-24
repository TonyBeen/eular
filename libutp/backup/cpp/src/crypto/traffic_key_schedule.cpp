/*************************************************************************
    > File Name: traffic_key_schedule.cpp
    > Brief: Direction-separated traffic key derivation for a UTP session.
 ************************************************************************/

#include "crypto/traffic_key_schedule.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <openssl/hkdf.h>
#include <openssl/mem.h>
#include <openssl/sha.h>

#include "proto/proto.h"
#include "utp/errno.h"

namespace eular {
namespace utp {

namespace {

const uint8_t kSaltLabel[] = "libutp-handshake-v2";
const uint8_t kInfoLabel[] = "libutp-traffic-keys-v2";

void StoreBe32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>((value >> 24) & 0xffu);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xffu);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xffu);
    out[3] = static_cast<uint8_t>(value & 0xffu);
}

bool IsAllZero(const X25519Wrapper::SharedSecret &secret)
{
    uint8_t aggregate = 0;
    for (uint8_t byte : secret) {
        aggregate |= byte;
    }
    return aggregate == 0;
}

} // namespace

Status TrafficKeySchedule::Derive(const X25519Wrapper::SharedSecret &sharedSecret,
                                  const X25519Wrapper::PublicKey &clientPublicKey,
                                  const X25519Wrapper::PublicKey &serverPublicKey,
                                  uint32_t clientCid,
                                  uint32_t serverCid,
                                  uint8_t cryptoType,
                                  size_t keySize,
                                  Material &out)
{
    out = Material{};
    if ((keySize != 16 && keySize != 32) || clientCid == 0 || serverCid == 0 || IsAllZero(sharedSecret)) {
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_INIT_FAILED, "invalid traffic key derivation input");
    }

    std::array<uint8_t, 4 + 4 + 4 + 1 + 32 + 32> transcript{};
    size_t offset = 0;
    StoreBe32(transcript.data() + offset, UTP_PROTOCOL_VERSION);
    offset += 4;
    StoreBe32(transcript.data() + offset, clientCid);
    offset += 4;
    StoreBe32(transcript.data() + offset, serverCid);
    offset += 4;
    transcript[offset++] = cryptoType;
    std::memcpy(transcript.data() + offset, clientPublicKey.data(), clientPublicKey.size());
    offset += clientPublicKey.size();
    std::memcpy(transcript.data() + offset, serverPublicKey.data(), serverPublicKey.size());

    std::array<uint8_t, SHA256_DIGEST_LENGTH> transcriptHash{};
    SHA256(transcript.data(), transcript.size(), transcriptHash.data());

    std::array<uint8_t, sizeof(kSaltLabel) - 1 + SHA256_DIGEST_LENGTH> saltInput{};
    std::memcpy(saltInput.data(), kSaltLabel, sizeof(kSaltLabel) - 1);
    std::memcpy(saltInput.data() + sizeof(kSaltLabel) - 1, transcriptHash.data(), transcriptHash.size());
    std::array<uint8_t, SHA256_DIGEST_LENGTH> salt{};
    SHA256(saltInput.data(), saltInput.size(), salt.data());

    std::array<uint8_t, sizeof(kInfoLabel) - 1 + SHA256_DIGEST_LENGTH> info{};
    std::memcpy(info.data(), kInfoLabel, sizeof(kInfoLabel) - 1);
    std::memcpy(info.data() + sizeof(kInfoLabel) - 1, transcriptHash.data(), transcriptHash.size());

    std::array<uint8_t, 2 * (MAX_KEY_SIZE + NONCE_PREFIX_SIZE)> expanded{};
    const size_t secretSize = keySize + NONCE_PREFIX_SIZE;
    const size_t outputSize = 2 * secretSize;
    if (HKDF(expanded.data(), outputSize, EVP_sha256(), sharedSecret.data(), sharedSecret.size(), salt.data(),
             salt.size(), info.data(), info.size()) != 1) {
        OPENSSL_cleanse(expanded.data(), expanded.size());
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_INIT_FAILED, "HKDF traffic key derivation failed");
    }

    std::copy_n(expanded.data(), keySize, out.clientToServer.key.data());
    std::copy_n(expanded.data() + keySize, NONCE_PREFIX_SIZE, out.clientToServer.noncePrefix.data());
    std::copy_n(expanded.data() + secretSize, keySize, out.serverToClient.key.data());
    std::copy_n(expanded.data() + secretSize + keySize, NONCE_PREFIX_SIZE,
                out.serverToClient.noncePrefix.data());
    out.keySize = keySize;

    OPENSSL_cleanse(expanded.data(), expanded.size());
    return Status::OK();
}

Status TrafficKeySchedule::CreateAesGcmContexts(const X25519Wrapper &x25519,
                                                const X25519Wrapper::PublicKey &peerPublicKey,
                                                uint32_t localCid,
                                                uint32_t peerCid,
                                                bool localIsClient,
                                                uint8_t cryptoType,
                                                size_t keySize,
                                                std::shared_ptr<AesGcmContext> &tx,
                                                std::shared_ptr<AesGcmContext> &rx)
{
    tx.reset();
    rx.reset();

    X25519Wrapper::SharedSecret sharedSecret = x25519.deriveSharedSecret(peerPublicKey);
    const X25519Wrapper::PublicKey &localPublicKey = x25519.publicKey();
    const X25519Wrapper::PublicKey &clientPublicKey = localIsClient ? localPublicKey : peerPublicKey;
    const X25519Wrapper::PublicKey &serverPublicKey = localIsClient ? peerPublicKey : localPublicKey;
    const uint32_t clientCid = localIsClient ? localCid : peerCid;
    const uint32_t serverCid = localIsClient ? peerCid : localCid;

    Material material;
    Status status = Derive(sharedSecret, clientPublicKey, serverPublicKey, clientCid, serverCid, cryptoType, keySize,
                           material);
    OPENSSL_cleanse(sharedSecret.data(), sharedSecret.size());
    if (!status.ok()) {
        return status;
    }

    const Secret &txSecret = localIsClient ? material.clientToServer : material.serverToClient;
    const Secret &rxSecret = localIsClient ? material.serverToClient : material.clientToServer;
    std::shared_ptr<AesGcmContext> newTx = std::make_shared<AesGcmContext>();
    std::shared_ptr<AesGcmContext> newRx = std::make_shared<AesGcmContext>();

    if (keySize == 32) {
        AesGcmContext::AesKey256 txKey{};
        AesGcmContext::AesKey256 rxKey{};
        std::copy_n(txSecret.key.data(), txKey.size(), txKey.data());
        std::copy_n(rxSecret.key.data(), rxKey.size(), rxKey.data());
        status = newTx->init(txKey, txSecret.noncePrefix);
        if (status.ok()) {
            status = newRx->init(rxKey, rxSecret.noncePrefix);
        }
        OPENSSL_cleanse(txKey.data(), txKey.size());
        OPENSSL_cleanse(rxKey.data(), rxKey.size());
    } else {
        AesGcmContext::AesKey128 txKey{};
        AesGcmContext::AesKey128 rxKey{};
        std::copy_n(txSecret.key.data(), txKey.size(), txKey.data());
        std::copy_n(rxSecret.key.data(), rxKey.size(), rxKey.data());
        status = newTx->init(txKey, txSecret.noncePrefix);
        if (status.ok()) {
            status = newRx->init(rxKey, rxSecret.noncePrefix);
        }
        OPENSSL_cleanse(txKey.data(), txKey.size());
        OPENSSL_cleanse(rxKey.data(), rxKey.size());
    }

    OPENSSL_cleanse(&material, sizeof(material));
    if (!status.ok()) {
        return status;
    }

    tx = newTx;
    rx = newRx;
    return Status::OK();
}

} // namespace utp
} // namespace eular
