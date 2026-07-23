/*************************************************************************
    > File Name: aes_gcm_context.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 06 Jan 2026 04:49:21 PM CST
 ************************************************************************/

#include "aes_gcm_context.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <utils/endian.hpp>
#include <openssl/mem.h>
#include <utp/errno.h>
#include <util/error.h>

#include "util/fiu_local.h"

#include "proto/proto.h"
#include "proto/packet_in.h"
#include "proto/packet_out.h"

namespace {

constexpr size_t kPayloadLengthOffset = 4 + 4 + 8;
constexpr size_t kEncryptBufferClassCount = 5;
constexpr size_t kEncryptBufferCacheLimitPerClass = 8;
constexpr size_t kEncryptBufferClasses[kEncryptBufferClassCount] = {
    1280,
    1500,
    4096,
    9000,
    65535,
};

struct EncryptBufferNode {
    SLIST_ENTRY(EncryptBufferNode)  next;
};

SLIST_HEAD(EncryptBufferFreeList, EncryptBufferNode);
struct EncryptBufferPool {
    EncryptBufferFreeList lists[kEncryptBufferClassCount];
    size_t                 counts[kEncryptBufferClassCount]{};

    EncryptBufferPool()
    {
        for (size_t i = 0; i < kEncryptBufferClassCount; ++i) {
            SLIST_INIT(&lists[i]);
        }
    }

    ~EncryptBufferPool()
    {
        purge();
    }

    void purge()
    {
        for (size_t i = 0; i < kEncryptBufferClassCount; ++i) {
            EncryptBufferNode* node = nullptr;
            while ((node = SLIST_FIRST(&lists[i])) != nullptr) {
                SLIST_REMOVE_HEAD(&lists[i], next);
                std::free(node);
            }
            counts[i] = 0;
        }
    }

    size_t count() const
    {
        size_t total = 0;
        for (size_t i = 0; i < kEncryptBufferClassCount; ++i) {
            total += counts[i];
        }
        return total;
    }
};
thread_local EncryptBufferPool g_encryptBufferPool;

size_t NormalizeEncryptBufferSize(size_t size)
{
    if (size == 0) {
        return 0;
    }

    for (size_t i = 0; i < kEncryptBufferClassCount; ++i) {
        if (size <= kEncryptBufferClasses[i]) {
            return kEncryptBufferClasses[i];
        }
    }

    return size;
}

bool IsPooledEncryptBufferSize(size_t size)
{
    return size > 0 && size <= kEncryptBufferClasses[kEncryptBufferClassCount - 1];
}

size_t EncryptBufferClassIndex(size_t capacity)
{
    for (size_t i = 0; i < kEncryptBufferClassCount; ++i) {
        if (capacity <= kEncryptBufferClasses[i]) {
            return i;
        }
    }
    return kEncryptBufferClassCount - 1;
}

void StoreBE16(uint8_t *dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    dst[1] = static_cast<uint8_t>(value & 0xff);
}

}

namespace eular {
namespace utp {
AesGcmContext::~AesGcmContext()
{
    cleanup();
}

Status AesGcmContext::init(const AesKey128 &key, uint32_t noncePerfix)
{
    noncePerfix = htobe32(noncePerfix);
    NoncePrefix prefix{};
    std::memcpy(prefix.data(), &noncePerfix, sizeof(noncePerfix));
    return init(key, prefix);
}

Status AesGcmContext::init(const AesKey128 &key, const NoncePrefix &noncePrefix)
{
    cleanup();

    m_cipher = EVP_aes_128_gcm();
    if (m_cipher == nullptr) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        return Status::Error(UTP_ERR_CRYPTO_INIT_FAILED,
                             fmt::format("AES-128-GCM init failed: {} (code=0x{:X})",
                                         msg.data(),
                                         sslCode));
    }

    m_keySize = key.size();
    std::copy(key.begin(), key.end(), m_key.begin());

    m_noncePerfix = noncePrefix;
    return Status::OK();
}

Status AesGcmContext::init(const AesKey256 &key, uint32_t noncePerfix)
{
    noncePerfix = htobe32(noncePerfix);
    NoncePrefix prefix{};
    std::memcpy(prefix.data(), &noncePerfix, sizeof(noncePerfix));
    return init(key, prefix);
}

Status AesGcmContext::init(const AesKey256 &key, const NoncePrefix &noncePrefix)
{
    cleanup();

    m_cipher = EVP_aes_256_gcm();
    if (m_cipher == nullptr) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        return Status::Error(UTP_ERR_CRYPTO_INIT_FAILED,
                             fmt::format("AES-256-GCM init failed: {} (code=0x{:X})",
                                         msg.data(),
                                         sslCode));
    }

    m_keySize = key.size();
    std::copy(key.begin(), key.end(), m_key.begin());

    m_noncePerfix = noncePrefix;
    return Status::OK();
}

Status AesGcmContext::encrypt(PacketOut *packet)
{
    if (packet == nullptr || packet->raw_data == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid packet for encrypt");
    }

    if (packet->data_size < UTP_HEADER_SIZE) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("packet too small for encrypt: data_size={}",
                                         packet->data_size));
    }

    const size_t plainPayloadLen = static_cast<size_t>(packet->data_size - UTP_HEADER_SIZE);
    const size_t cipherPayloadLen = plainPayloadLen + GCM_TAG_SIZE;
    const size_t encryptedPacketLen = UTP_HEADER_SIZE + cipherPayloadLen;
    if (encryptedPacketLen > (std::numeric_limits<uint16_t>::max)()) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("encrypted packet too large: {}",
                                         encryptedPacketLen));
    }

    uint8_t *targetBuffer = nullptr;
    bool inPlace = false;

    const bool keepPlaintext = (packet->po_flags & PacketOutFlags::kPoKeepPlaintext) != 0;

    if (!keepPlaintext && packet->alloc_size >= encryptedPacketLen) {
        targetBuffer = packet->raw_data;
        inPlace = true;
    } else {
        targetBuffer = AcquireEncryptBuffer(encryptedPacketLen);
        if (targetBuffer == nullptr) {
            return Status::Error(UTP_ERR_NO_MEMORY,
                                 fmt::format("allocate encrypted packet buffer failed, size={}",
                                             encryptedPacketLen));
        }
        std::memcpy(targetBuffer, packet->raw_data, UTP_HEADER_SIZE);
    }

    StoreBE16(targetBuffer + kPayloadLengthOffset, static_cast<uint16_t>(cipherPayloadLen));

    size_t outCipherPayloadLen = cipherPayloadLen;
    // For in-place encryption, plaintext and ciphertext pointers are the same.
    const Status encStatus = encrypt(packet->raw_data + UTP_HEADER_SIZE,
                                     plainPayloadLen,
                                     targetBuffer,
                                     UTP_HEADER_SIZE,
                                     packet->packno,
                                     targetBuffer + UTP_HEADER_SIZE,
                                     &outCipherPayloadLen);
    if (!encStatus.ok()) {
        if (!inPlace) {
            ReleaseEncryptBuffer(targetBuffer, encryptedPacketLen);
        }
        return encStatus;
    }

    if (packet->encrypt_data != nullptr && packet->encrypt_data != packet->raw_data) {
        ReleaseEncryptBuffer(packet->encrypt_data, packet->encrypt_data_size);
    }

    packet->encrypt_data = targetBuffer;
    packet->encrypt_data_size = static_cast<uint16_t>(UTP_HEADER_SIZE + outCipherPayloadLen);
    packet->po_flags |= PacketOutFlags::kPoEncrypted;
    return Status::OK();
}

Status AesGcmContext::decrypt(PacketIn *packet)
{
    if (packet == nullptr || packet->raw_data == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid packet for decrypt");
    }

    if (packet->raw_size < UTP_HEADER_SIZE) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("packet too small for decrypt: raw_size={}",
                                         packet->raw_size));
    }

    if (packet->payload == nullptr) {
        if (packet->raw_size < UTP_HEADER_SIZE) {
            return Status::Error(UTP_ERR_OVERFLOW,
                                 fmt::format("invalid encrypted packet raw_size={}", packet->raw_size));
        }

        packet->payload = packet->raw_data + UTP_HEADER_SIZE;
        packet->payload_size = packet->raw_size - UTP_HEADER_SIZE;
    }

    if (packet->payload_size < GCM_TAG_SIZE) {
        return Status::Error(UTP_ERR_CRYPTO_DECRYPTION,
                             fmt::format("encrypted payload too short: {}",
                                         packet->payload_size));
    }

    uint8_t *mutablePayload = const_cast<uint8_t *>(packet->payload);
    size_t plainPayloadLen = packet->payload_size - GCM_TAG_SIZE;
    const Status decStatus = decrypt(packet->payload,
                                     packet->payload_size,
                                     packet->raw_data,
                                     UTP_HEADER_SIZE,
                                     packet->header.pn,
                                     mutablePayload,
                                     &plainPayloadLen);
    if (!decStatus.ok()) {
        return decStatus;
    }

    packet->header.payload_length = static_cast<uint16_t>(plainPayloadLen);
    packet->payload_size = plainPayloadLen;
    packet->frame_types = 0;

    uint8_t *mutableRaw = const_cast<uint8_t *>(packet->raw_data);
    StoreBE16(mutableRaw + kPayloadLengthOffset, packet->header.payload_length);

    if (packet->raw_size >= (UTP_HEADER_SIZE + plainPayloadLen + GCM_TAG_SIZE)) {
        packet->raw_size = UTP_HEADER_SIZE + plainPayloadLen;
    }

    return Status::OK();
}

Status AesGcmContext::encrypt(const uint8_t *plaintext, size_t plaintext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *ciphertext, size_t *ciphertext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
    }

    if (ciphertext == nullptr || ciphertext_len == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "ciphertext buffer is null");
    }

    if (plaintext_len > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "encrypt input is too large");
    }

    const size_t required = plaintext_len + GCM_TAG_SIZE;
    if (*ciphertext_len < required) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("ciphertext buffer too small: required={}, provided={}",
                                         required,
                                         *ciphertext_len));
    }

    Nonce nonce = buildNonce(counter);

#if defined(UTP_ENABLE_FAULT_INJECTION)
    if (fiu_fail("crypto/evp_ctx/alloc")) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
    }
#endif
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
    }

    int32_t status = EVP_EncryptInit_ex(ctx, m_cipher, nullptr, nullptr, nullptr);
    status = (status == 1) ? EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) : 0;
    status = (status == 1) ? EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_key.data(), nonce.data()) : 0;

    int32_t outLen = 0;
    int32_t totalLen = 0;
    if (status == 1 && aad != nullptr && aad_len > 0) {
        status = EVP_EncryptUpdate(ctx, nullptr, &outLen, aad, static_cast<int32_t>(aad_len));
    }
    if (status == 1 && plaintext != nullptr && plaintext_len > 0) {
        status = EVP_EncryptUpdate(ctx,
                                   ciphertext,
                                   &outLen,
                                   plaintext,
                                   static_cast<int32_t>(plaintext_len));
        totalLen += outLen;
    }
    if (status == 1) {
        status = EVP_EncryptFinal_ex(ctx, ciphertext + totalLen, &outLen);
        totalLen += outLen;
    }
    if (status == 1) {
        status = EVP_CIPHER_CTX_ctrl(ctx,
                                     EVP_CTRL_GCM_GET_TAG,
                                     GCM_TAG_SIZE,
                                     ciphertext + totalLen);
    }

    EVP_CIPHER_CTX_free(ctx);
    if (status != 1) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        return Status::Error(UTP_ERR_CRYPTO_ENCRYPTION,
                             fmt::format("AES-GCM encryption failed: {} (code=0x{:X})",
                                         msg.data(),
                                         sslCode));
    }

    *ciphertext_len = static_cast<size_t>(totalLen + GCM_TAG_SIZE);

    return Status::OK();
}

Status AesGcmContext::encryptScatter(const PlainSegment *segments,
                                     size_t segmentCount,
                                     const uint8_t *aad,
                                     size_t aad_len,
                                     uint64_t counter,
                                     uint8_t *ciphertext,
                                     size_t *ciphertext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
    }

    if (ciphertext == nullptr || ciphertext_len == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "ciphertext buffer is null");
    }

    size_t totalPlainLen = 0;
    if (segmentCount > 0 && segments == nullptr) {
        return Status::Error(UTP_ERR_INVALID_PARAM,
                             fmt::format("segments is null when segmentCount={}", segmentCount));
    }
    for (size_t i = 0; i < segmentCount; ++i) {
        if (segments[i].len == 0) {
            continue;
        }
        if (segments[i].data == nullptr) {
            return Status::Error(UTP_ERR_INVALID_PARAM,
                                 fmt::format("segment {} data is null", i));
        }
        totalPlainLen += segments[i].len;
    }

    if (totalPlainLen > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "encrypt input is too large");
    }

    const size_t required = totalPlainLen + GCM_TAG_SIZE;
    if (*ciphertext_len < required) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("ciphertext buffer too small: required={}, provided={}",
                                         required,
                                         *ciphertext_len));
    }

    Nonce nonce = buildNonce(counter);

#if defined(UTP_ENABLE_FAULT_INJECTION)
    if (fiu_fail("crypto/evp_ctx/alloc")) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
    }
#endif
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
    }

    int32_t status = EVP_EncryptInit_ex(ctx, m_cipher, nullptr, nullptr, nullptr);
    status = (status == 1) ? EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) : 0;
    status = (status == 1) ? EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_key.data(), nonce.data()) : 0;

    int32_t outLen = 0;
    int32_t totalLen = 0;
    if (status == 1 && aad != nullptr && aad_len > 0) {
        status = EVP_EncryptUpdate(ctx, nullptr, &outLen, aad, static_cast<int32_t>(aad_len));
    }
    for (size_t i = 0; status == 1 && i < segmentCount; ++i) {
        if (segments[i].data == nullptr || segments[i].len == 0) {
            continue;
        }
        status = EVP_EncryptUpdate(ctx,
                                   ciphertext + totalLen,
                                   &outLen,
                                   segments[i].data,
                                   static_cast<int32_t>(segments[i].len));
        totalLen += outLen;
    }
    if (status == 1) {
        status = EVP_EncryptFinal_ex(ctx, ciphertext + totalLen, &outLen);
        totalLen += outLen;
    }
    if (status == 1) {
        status = EVP_CIPHER_CTX_ctrl(ctx,
                                     EVP_CTRL_GCM_GET_TAG,
                                     GCM_TAG_SIZE,
                                     ciphertext + totalLen);
    }

    EVP_CIPHER_CTX_free(ctx);
    if (status != 1) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        return Status::Error(UTP_ERR_CRYPTO_ENCRYPTION,
                             fmt::format("AES-GCM scatter encryption failed: {} (code=0x{:X})",
                                         msg.data(),
                                         sslCode));
    }

    *ciphertext_len = static_cast<size_t>(totalLen + GCM_TAG_SIZE);
    return Status::OK();
}

Status AesGcmContext::decrypt(const uint8_t *ciphertext, size_t ciphertext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *plaintext, size_t *plaintext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
    }

    if (plaintext == nullptr || plaintext_len == nullptr || ciphertext == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "decrypt buffer arg is null");
    }

    if (ciphertext_len > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "decrypt input is too large");
    }

    if (ciphertext_len < GCM_TAG_SIZE) {
        return Status::ErrorLiteral(UTP_ERR_CRYPTO_DECRYPTION, "Ciphertext too short for AES-GCM decryption");
    }

    const size_t cipherDataLen = ciphertext_len - GCM_TAG_SIZE;
    if (*plaintext_len < cipherDataLen) {
        return Status::Error(UTP_ERR_OVERFLOW,
                             fmt::format("plaintext buffer too small: required={}, provided={}",
                                         cipherDataLen,
                                         *plaintext_len));
    }

    Nonce nonce = buildNonce(counter);

    const uint8_t *tag = ciphertext + cipherDataLen;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
    }

    int32_t status = EVP_DecryptInit_ex(ctx, m_cipher, nullptr, nullptr, nullptr);
    status = (status == 1) ? EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) : 0;
    status = (status == 1) ? EVP_DecryptInit_ex(ctx, nullptr, nullptr, m_key.data(), nonce.data()) : 0;

    int32_t outLen = 0;
    int32_t totalLen = 0;
    if (status == 1 && aad != nullptr && aad_len > 0) {
        status = EVP_DecryptUpdate(ctx, nullptr, &outLen, aad, static_cast<int32_t>(aad_len));
    }
    if (status == 1 && cipherDataLen > 0) {
        status = EVP_DecryptUpdate(ctx,
                                   plaintext,
                                   &outLen,
                                   ciphertext,
                                   static_cast<int32_t>(cipherDataLen));
        totalLen += outLen;
    }
    if (status == 1) {
        status = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_SIZE, const_cast<uint8_t *>(tag));
    }
    if (status == 1) {
        status = EVP_DecryptFinal_ex(ctx, plaintext + totalLen, &outLen);
        totalLen += outLen;
    }

    EVP_CIPHER_CTX_free(ctx);
    if (status != 1) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        return Status::Error(UTP_ERR_CRYPTO_DECRYPTION,
                             fmt::format("AES-GCM decryption failed: {} (code=0x{:X})",
                                         msg.data(),
                                         sslCode));
    }

    *plaintext_len = static_cast<size_t>(totalLen);

    return Status::OK();
}

AesGcmContext::Nonce AesGcmContext::buildNonce(uint64_t counter) const
{
    Nonce nonce;
    std::memcpy(nonce.data(), m_noncePerfix.data(), m_noncePerfix.size());

    counter = htobe64(counter);
    std::memcpy(nonce.data() + m_noncePerfix.size(), &counter, sizeof(counter));
    return nonce;
}

void AesGcmContext::cleanup()
{
    m_cipher = nullptr;
    m_keySize = 0;
    OPENSSL_cleanse(m_key.data(), m_key.size());
    OPENSSL_cleanse(m_noncePerfix.data(), m_noncePerfix.size());
}

uint8_t *AesGcmContext::AcquireEncryptBuffer(size_t size)
{
    if (size == 0) {
        return nullptr;
    }

    if (!IsPooledEncryptBufferSize(size)) {
        return static_cast<uint8_t *>(std::malloc(size));
    }

    const size_t capacity = NormalizeEncryptBufferSize(size);
    const size_t idx = EncryptBufferClassIndex(capacity);
    EncryptBufferNode *node = SLIST_FIRST(&g_encryptBufferPool.lists[idx]);
    if (node != nullptr) {
        SLIST_REMOVE_HEAD(&g_encryptBufferPool.lists[idx], next);
        --g_encryptBufferPool.counts[idx];
        return reinterpret_cast<uint8_t *>(node);
    }

    return static_cast<uint8_t *>(std::malloc(capacity));
}

void AesGcmContext::ReleaseEncryptBuffer(uint8_t *buffer, size_t size)
{
    if (buffer == nullptr || size == 0) {
        return;
    }

    if (!IsPooledEncryptBufferSize(size)) {
        std::free(buffer);
        return;
    }

    const size_t capacity = NormalizeEncryptBufferSize(size);
    const size_t idx = EncryptBufferClassIndex(capacity);
    if (g_encryptBufferPool.counts[idx] >= kEncryptBufferCacheLimitPerClass) {
        std::free(buffer);
        return;
    }
    EncryptBufferNode *node = reinterpret_cast<EncryptBufferNode *>(buffer);
    SLIST_INSERT_HEAD(&g_encryptBufferPool.lists[idx], node, next);
    ++g_encryptBufferPool.counts[idx];
}

void AesGcmContext::PurgeThreadLocalEncryptBuffers()
{
    g_encryptBufferPool.purge();
}

size_t AesGcmContext::CachedThreadLocalEncryptBufferCount()
{
    return g_encryptBufferPool.count();
}

} // namespace utp
} // namespace eular
