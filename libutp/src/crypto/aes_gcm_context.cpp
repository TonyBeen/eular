/*************************************************************************
    > File Name: aes_gcm_context.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 06 Jan 2026 04:49:21 PM CST
 ************************************************************************/

#include "aes_gcm_context.h"

#include <cstring>

#include <utils/endian.hpp>
#include <util/error.h>

namespace eular {
namespace utp {
AesGcmContext::~AesGcmContext()
{
    cleanup();
}

bool AesGcmContext::init(const AesKey128 &key, uint32_t noncePerfix)
{
    cleanup();

    m_ctx = EVP_AEAD_CTX_new(EVP_aead_aes_128_gcm(), key.data(), key.size(), GCM_TAG_SIZE);
    if (!m_ctx) {
        uint32_t code = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(code);
        SetLastErrorV(UTP_ERR_CRYPTO_INIT_FAILED, "AES-GCM context initialization failed: {} (code=0x{:X})", msg.data(), code);
        return false;
    }

    noncePerfix = htobe32(noncePerfix);
    std::memcpy(m_noncePerfix.data(), &noncePerfix, sizeof(noncePerfix));
}

bool AesGcmContext::init(const AesKey256 &key, uint32_t noncePerfix)
{
    cleanup();

    m_ctx = EVP_AEAD_CTX_new(EVP_aead_aes_256_gcm(), key.data(), key.size(), GCM_TAG_SIZE);
    if (!m_ctx) {
        uint32_t code = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(code);
        SetLastErrorV(UTP_ERR_CRYPTO_INIT_FAILED, "AES-GCM context initialization failed: {} (code=0x{:X})", msg.data(), code);
        return false;
    }

    noncePerfix = htobe32(noncePerfix);
    std::memcpy(m_noncePerfix.data(), &noncePerfix, sizeof(noncePerfix));
}

int32_t AesGcmContext::encrypt(const uint8_t *plaintext, size_t plaintext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *ciphertext, size_t *ciphertext_len)
{
    if (!m_ctx) {
        SetLastErrorV(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
        return -1;
    }

    Nonce nonce = buildNonce(counter);
    int32_t code = EVP_AEAD_CTX_seal(m_ctx,
                      ciphertext, ciphertext_len, *ciphertext_len,
                      nonce.data(), nonce.size(),
                      plaintext, plaintext_len,
                      aad, aad_len);
    if (code != 1) {
        uint32_t code = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(code);
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "AES-GCM encryption failed: {} (code=0x{:X})", msg.data(), code);
        return -1;
    }

    return 0;
}

int32_t AesGcmContext::decrypt(const uint8_t *ciphertext, size_t ciphertext_len, const uint8_t *aad, size_t aad_len, uint64_t counter, uint8_t *plaintext, size_t *plaintext_len)
{
    if (!m_ctx) {
        SetLastErrorV(UTP_ERR_CRYPTO_UNINITIALIZED, "Crypto context uninitialized");
        return -1;
    }

    if (ciphertext_len < GCM_TAG_SIZE) {
        SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "Ciphertext too short for AES-GCM decryption");
        return -1;
    }

    Nonce nonce = buildNonce(counter);
    int32_t code = EVP_AEAD_CTX_open(m_ctx,
                        plaintext, plaintext_len, *plaintext_len,
                        nonce.data(), nonce.size(),
                        ciphertext, ciphertext_len,
                        aad, aad_len);
    if (code != 1) {
        uint32_t code = 0;
        OpenSSLErrorMsg msg = GetOpenSSLErrorMsg(code);
        SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "AES-GCM decryption failed: {} (code=0x{:X})", msg.data(), code);
        return -1;
    }

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
    if (m_ctx) {
        EVP_AEAD_CTX_free(m_ctx);
        m_ctx = nullptr;

        std::memset(m_noncePerfix.data(), 0, m_noncePerfix.size());
    }
}

} // namespace utp
} // namespace eular
