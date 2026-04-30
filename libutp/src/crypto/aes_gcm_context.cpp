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
#include <utp/errno.h>
#include <util/error.h>

#include "proto/proto.h"
#include "proto/packet_in.h"
#include "proto/packet_out.h"

namespace {

constexpr size_t kPayloadLengthOffset = 4 + 4 + 8;

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

bool AesGcmContext::init(const AesKey128 &key, uint32_t noncePerfix)
{
    cleanup();

    m_cipher = EVP_aes_128_gcm();
    if (m_cipher == nullptr) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        SetLastErrorV(UTP_ERR_CRYPTO_INIT_FAILED,
                      "AES-128-GCM init failed: {} (code=0x{:X})",
                      msg.data(),
                      sslCode);
        return false;
    }

    m_keySize = key.size();
    std::copy(key.begin(), key.end(), m_key.begin());

    noncePerfix = htobe32(noncePerfix);
    std::memcpy(m_noncePerfix.data(), &noncePerfix, sizeof(noncePerfix));
    return true;
}

bool AesGcmContext::init(const AesKey256 &key, uint32_t noncePerfix)
{
    cleanup();

    m_cipher = EVP_aes_256_gcm();
    if (m_cipher == nullptr) {
        uint32_t sslCode = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(sslCode);
        SetLastErrorV(UTP_ERR_CRYPTO_INIT_FAILED,
                      "AES-256-GCM init failed: {} (code=0x{:X})",
                      msg.data(),
                      sslCode);
        return false;
    }

    m_keySize = key.size();
    std::copy(key.begin(), key.end(), m_key.begin());

    noncePerfix = htobe32(noncePerfix);
    std::memcpy(m_noncePerfix.data(), &noncePerfix, sizeof(noncePerfix));
    return true;
}

int32_t AesGcmContext::encrypt(PacketOut *packet)
{
    if (packet == nullptr || packet->raw_data == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "invalid packet for encrypt");
        return -1;
    }

    if (packet->data_size < UTP_HEADER_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "packet too small for encrypt: data_size={}",
                      packet->data_size);
        return -1;
    }

    const size_t plainPayloadLen = static_cast<size_t>(packet->data_size - UTP_HEADER_SIZE);
    const size_t cipherPayloadLen = plainPayloadLen + GCM_TAG_SIZE;
    const size_t encryptedPacketLen = UTP_HEADER_SIZE + cipherPayloadLen;
    if (encryptedPacketLen > (std::numeric_limits<uint16_t>::max)()) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "encrypted packet too large: {}",
                      encryptedPacketLen);
        return -1;
    }

    uint8_t *targetBuffer = nullptr;
    bool inPlace = false;

    const bool keepPlaintext = (packet->po_flags & PacketOutFlags::kPoKeepPlaintext) != 0;

    if (!keepPlaintext && packet->alloc_size >= encryptedPacketLen) {
        targetBuffer = packet->raw_data;
        inPlace = true;
    } else {
        targetBuffer = static_cast<uint8_t *>(std::malloc(encryptedPacketLen));
        if (targetBuffer == nullptr) {
            SetLastErrorV(UTP_ERR_NO_MEMORY,
                          "allocate encrypted packet buffer failed, size={}",
                          encryptedPacketLen);
            return -1;
        }
        std::memcpy(targetBuffer, packet->raw_data, UTP_HEADER_SIZE);
    }

    StoreBE16(targetBuffer + kPayloadLengthOffset, static_cast<uint16_t>(cipherPayloadLen));

    size_t outCipherPayloadLen = cipherPayloadLen;
    // For in-place encryption, plaintext and ciphertext pointers are the same.
    const int32_t status = encrypt(packet->raw_data + UTP_HEADER_SIZE,
                                   plainPayloadLen,
                                   targetBuffer,
                                   UTP_HEADER_SIZE,
                                   packet->packno,
                                   targetBuffer + UTP_HEADER_SIZE,
                                   &outCipherPayloadLen);
    if (status < 0) {
        if (!inPlace) {
            std::free(targetBuffer);
        }
        return -1;
    }

    if (packet->encrypt_data != nullptr && packet->encrypt_data != packet->raw_data) {
        std::free(packet->encrypt_data);
    }

    packet->encrypt_data = targetBuffer;
    packet->encrypt_data_size = static_cast<uint16_t>(UTP_HEADER_SIZE + outCipherPayloadLen);
    packet->po_flags |= PacketOutFlags::kPoEncrypted;
    return UTP_ERR_OK;
}

int32_t AesGcmContext::decrypt(PacketIn *packet)
{
    if (packet == nullptr || packet->raw_data == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "invalid packet for decrypt");
        return -1;
    }

    if (packet->raw_size < UTP_HEADER_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "packet too small for decrypt: raw_size={}",
                      packet->raw_size);
        return -1;
    }

    if (packet->payload == nullptr) {
        if (packet->raw_size < UTP_HEADER_SIZE) {
            SetLastErrorV(UTP_ERR_OVERFLOW, "invalid encrypted packet raw_size={}", packet->raw_size);
            return -1;
        }

        packet->payload = packet->raw_data + UTP_HEADER_SIZE;
        packet->payload_size = packet->raw_size - UTP_HEADER_SIZE;
    }

    if (packet->payload_size < GCM_TAG_SIZE) {
        SetLastErrorV(UTP_ERR_CRYPTO_DECRYPTION,
                      "encrypted payload too short: {}",
                      packet->payload_size);
        return -1;
    }

    uint8_t *mutablePayload = const_cast<uint8_t *>(packet->payload);
    size_t plainPayloadLen = packet->payload_size - GCM_TAG_SIZE;
    const int32_t status = decrypt(packet->payload,
                                   packet->payload_size,
                                   packet->raw_data,
                                   UTP_HEADER_SIZE,
                                   packet->header.pn,
                                   mutablePayload,
                                   &plainPayloadLen);
    if (status < 0) {
        return -1;
    }

    packet->header.payload_length = static_cast<uint16_t>(plainPayloadLen);
    packet->payload_size = plainPayloadLen;
    packet->frame_types = 0;

    uint8_t *mutableRaw = const_cast<uint8_t *>(packet->raw_data);
    StoreBE16(mutableRaw + kPayloadLengthOffset, packet->header.payload_length);

    if (packet->raw_size >= (UTP_HEADER_SIZE + plainPayloadLen + GCM_TAG_SIZE)) {
        packet->raw_size = UTP_HEADER_SIZE + plainPayloadLen;
    }

    return UTP_ERR_OK;
}

int32_t AesGcmContext::encrypt(const uint8_t *plaintext, size_t plaintext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *ciphertext, size_t *ciphertext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        SetLastErrorV(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
        return -1;
    }

    if (ciphertext == nullptr || ciphertext_len == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "ciphertext buffer is null");
        return -1;
    }

    if (plaintext_len > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encrypt input is too large");
        return -1;
    }

    const size_t required = plaintext_len + GCM_TAG_SIZE;
    if (*ciphertext_len < required) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "ciphertext buffer too small: required={}, provided={}",
                      required,
                      *ciphertext_len);
        return -1;
    }

    Nonce nonce = buildNonce(counter);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        SetLastErrorV(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
        return -1;
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
        SetLastErrorV(UTP_ERR_CRYPTO_ENCRYPTION,
                      "AES-GCM encryption failed: {} (code=0x{:X})",
                      msg.data(),
                      sslCode);
        return -1;
    }

    *ciphertext_len = static_cast<size_t>(totalLen + GCM_TAG_SIZE);

    return 0;
}

int32_t AesGcmContext::encryptScatter(const PlainSegment *segments,
                                     size_t segmentCount,
                                     const uint8_t *aad,
                                     size_t aad_len,
                                     uint64_t counter,
                                     uint8_t *ciphertext,
                                     size_t *ciphertext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        SetLastErrorV(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
        return -1;
    }

    if (ciphertext == nullptr || ciphertext_len == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "ciphertext buffer is null");
        return -1;
    }

    size_t totalPlainLen = 0;
    if (segmentCount > 0 && segments == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "segments is null when segmentCount={}", segmentCount);
        return -1;
    }
    for (size_t i = 0; i < segmentCount; ++i) {
        if (segments[i].len == 0) {
            continue;
        }
        if (segments[i].data == nullptr) {
            SetLastErrorV(UTP_ERR_INVALID_PARAM, "segment {} data is null", i);
            return -1;
        }
        totalPlainLen += segments[i].len;
    }

    if (totalPlainLen > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encrypt input is too large");
        return -1;
    }

    const size_t required = totalPlainLen + GCM_TAG_SIZE;
    if (*ciphertext_len < required) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "ciphertext buffer too small: required={}, provided={}",
                      required,
                      *ciphertext_len);
        return -1;
    }

    Nonce nonce = buildNonce(counter);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        SetLastErrorV(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
        return -1;
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
        SetLastErrorV(UTP_ERR_CRYPTO_ENCRYPTION,
                      "AES-GCM scatter encryption failed: {} (code=0x{:X})",
                      msg.data(),
                      sslCode);
        return -1;
    }

    *ciphertext_len = static_cast<size_t>(totalLen + GCM_TAG_SIZE);
    return 0;
}

int32_t AesGcmContext::decrypt(const uint8_t *ciphertext, size_t ciphertext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *plaintext, size_t *plaintext_len)
{
    if (m_cipher == nullptr || m_keySize == 0) {
        SetLastErrorV(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
        return -1;
    }

    if (plaintext == nullptr || plaintext_len == nullptr || ciphertext == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "decrypt buffer arg is null");
        return -1;
    }

    if (ciphertext_len > (std::numeric_limits<int>::max)() || aad_len > (std::numeric_limits<int>::max)()) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decrypt input is too large");
        return -1;
    }

    if (ciphertext_len < GCM_TAG_SIZE) {
        SetLastErrorV(UTP_ERR_CRYPTO_DECRYPTION, "Ciphertext too short for AES-GCM decryption");
        return -1;
    }

    const size_t cipherDataLen = ciphertext_len - GCM_TAG_SIZE;
    if (*plaintext_len < cipherDataLen) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "plaintext buffer too small: required={}, provided={}",
                      cipherDataLen,
                      *plaintext_len);
        return -1;
    }

    Nonce nonce = buildNonce(counter);

    const uint8_t *tag = ciphertext + cipherDataLen;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        SetLastErrorV(UTP_ERR_NO_MEMORY, "alloc EVP_CIPHER_CTX failed");
        return -1;
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
        SetLastErrorV(UTP_ERR_CRYPTO_DECRYPTION,
                      "AES-GCM decryption failed: {} (code=0x{:X})",
                      msg.data(),
                      sslCode);
        return -1;
    }

    *plaintext_len = static_cast<size_t>(totalLen);

    return 0;
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
    std::memset(m_key.data(), 0, m_key.size());
    std::memset(m_noncePerfix.data(), 0, m_noncePerfix.size());
}

} // namespace utp
} // namespace eular
