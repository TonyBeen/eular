/*************************************************************************
    > File Name: token.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 03 Feb 2026 04:27:29 PM CST
 ************************************************************************/

#include "crypto/token.h"
#include "token.h"

#include <utils/exception.h>
#include <utils/endian.hpp>
#include "util/error.h"

#ifndef TOKEN_KEY_UPDATE_INTERVAL_MS
#define TOKEN_KEY_UPDATE_INTERVAL_MS (60 * 60 * 1000) // 1 hour
#endif

namespace eular {
namespace utp {

static inline const EVP_CIPHER* AEADCipher()
{
    // OpenSSL: AES-256-GCM
    return EVP_aes_256_gcm();
}

TokenAuth::TokenAuth(event_base *base)
{
    m_timerUpdateKey.reset(base, std::bind(&TokenAuth::onUpdateKey, this));
    m_timerUpdateKey.start(TOKEN_KEY_UPDATE_INTERVAL_MS, TOKEN_KEY_UPDATE_INTERVAL_MS);
    if (RAND_bytes(m_key.data(), (int32_t)m_key.size()) != 1) {
        throw Exception("RAND_bytes failed");
    }
    m_oldKey = m_key;

    m_sealCtx = EVP_CIPHER_CTX_new();
    m_openCtx = EVP_CIPHER_CTX_new();
    if (!m_sealCtx || !m_openCtx) {
        throw Exception("EVP_CIPHER_CTX_new failed");
    }

    // int32_t status = 0;
    // int32_t outputSize = 0;
    // status = EVP_EncryptInit_ex(m_ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    // if (status != 1) {
    //     EVP_CIPHER_CTX_free(m_ctx);
    //     m_ctx = nullptr;
    //     throw Exception("EVP_EncryptInit_ex failed");
    // }

    // status = EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_GCM_SET_IVLEN, AEAD_NONCE_SIZE, nullptr);
    // if (status != 1) {
    //     EVP_CIPHER_CTX_free(m_ctx);
    //     m_ctx = nullptr;
    //     throw Exception("EVP_CIPHER_CTX_ctrl failed");
    // }

    // status = EVP_EncryptInit_ex(m_ctx, nullptr, nullptr, m_key.data(), nullptr);
    // if (status != 1) {
    //     EVP_CIPHER_CTX_free(m_ctx);
    //     m_ctx = nullptr;
    //     throw Exception("EVP_EncryptInit_ex failed");
    // }

    // // AAD
    // status = EVP_EncryptUpdate(m_ctx, nullptr, &outputSize, (const uint8_t *)(TOKEN_AAD.c_str()), TOKEN_AAD.size());
    // if (status != 1) {
    //     EVP_CIPHER_CTX_free(m_ctx);
    //     m_ctx = nullptr;
    //     throw Exception("EVP_EncryptUpdate failed");
    // }
}

TokenAuth::~TokenAuth()
{
    if (m_sealCtx) {
        EVP_CIPHER_CTX_free(m_sealCtx);
        m_sealCtx = nullptr;
    }

    if (m_openCtx) {
        EVP_CIPHER_CTX_free(m_openCtx);
        m_openCtx = nullptr;
    }
}

bool TokenAuth::seal(const TokenMeta &meta, TokenBuf &outToken)
{
    std::array<uint8_t, TOKEN_META_SIZE> plaintext;
    encode(meta, plaintext);

    // nonce
    uint8_t *nonce = outToken.data();
    if (RAND_bytes(nonce, AEAD_NONCE_SIZE) != 1) {
        SetLastErrorV(UTP_ERR_RANDOM_GENERATION_FAILED, "RAND_bytes failed");
        return false;
    }

    uint8_t* ciphertext = outToken.data() + AEAD_NONCE_SIZE;
    uint8_t* tag        = outToken.data() + AEAD_NONCE_SIZE + TOKEN_META_SIZE;

    EVP_CIPHER_CTX_reset(m_sealCtx);
    int32_t status = EVP_EncryptInit_ex(m_sealCtx, AEADCipher(), nullptr, m_key.data(), nonce);
    if (status != 1) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_EncryptInit_ex failed");
        return false;
    }
    status = EVP_CIPHER_CTX_ctrl(m_sealCtx, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE_SIZE, nullptr);
    if (status != 1) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_CIPHER_CTX_ctrl failed");
        return false;
    }
    status = EVP_EncryptInit_ex(m_sealCtx, nullptr, nullptr, m_key.data(), nonce);
    if (status != 1) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_EncryptInit_ex failed");
        return false;
    }
    // AAD
    status = EVP_EncryptUpdate(m_sealCtx, nullptr, nullptr, (const uint8_t *)(TOKEN_AAD.c_str()), static_cast<int32_t>(TOKEN_AAD.size()));
    if (status != 1) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_EncryptUpdate failed");
        return false;
    }

    // encrypt plaintext
    int32_t outLen = 0;
    status = EVP_EncryptUpdate(m_sealCtx, ciphertext, &outLen, plaintext.data(), static_cast<int32_t>(plaintext.size()));
    if (status != 1 || outLen != (int32_t)TOKEN_META_SIZE) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_EncryptUpdate failed");
        return false;
    }

    // finalize
    int32_t finalOutLen = 0;
    status = EVP_EncryptFinal_ex(m_sealCtx, ciphertext + outLen, &finalOutLen);
    if (status != 1 || finalOutLen != 0) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_EncryptFinal_ex failed");
        return false;
    }
    // get tag
    status = EVP_CIPHER_CTX_ctrl(m_sealCtx, EVP_CTRL_AEAD_GET_TAG, AEAD_TAG_SIZE, tag);
    if (status != 1) {
        SetLastErrorV(UTP_ERR_ENCRYPTION_ERROR, "EVP_CIPHER_CTX_ctrl failed");
        return false;
    }
    return true;
}

bool TokenAuth::open(const TokenBuf &token, TokenMeta &outMeta)
{
    const uint8_t *nonce      = token.data();
    const uint8_t *ciphertext = token.data() + AEAD_NONCE_SIZE;
    const uint8_t *tag        = token.data() + AEAD_NONCE_SIZE + TOKEN_META_SIZE;

    auto try_open_with_key = [&] (const TokenKey& key) -> bool {
        EVP_CIPHER_CTX_reset(m_openCtx);

        int32_t status = 0;
        int32_t outLen = 0;
        std::array<uint8_t, TOKEN_META_SIZE> plaintext;

        status = EVP_DecryptInit_ex(m_openCtx, AEADCipher(), nullptr, nullptr, nullptr);
        if (status != 1) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        status = EVP_CIPHER_CTX_ctrl(m_openCtx, EVP_CTRL_AEAD_SET_IVLEN, AEAD_NONCE_SIZE, nullptr);
        if (status != 1) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }
        status = EVP_DecryptInit_ex(m_openCtx, nullptr, nullptr, key.data(), nonce);
        if (status != 1) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        status = EVP_DecryptUpdate(m_openCtx, nullptr, &outLen, (const uint8_t *)(TOKEN_AAD.data()), (int32_t)(TOKEN_AAD.size()));
        if (status != 1) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        status = EVP_DecryptUpdate(m_openCtx, plaintext.data(), &outLen, ciphertext, (int32_t)(TOKEN_META_SIZE));
        if (status != 1 || outLen != static_cast<int32_t>(TOKEN_META_SIZE)) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        status = EVP_CIPHER_CTX_ctrl(m_openCtx, EVP_CTRL_AEAD_SET_TAG, AEAD_TAG_SIZE, (void *)tag);
        if (status != 1) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        int32_t finalLen = 0;
        status = EVP_DecryptFinal_ex(m_openCtx, plaintext.data() + outLen, &finalLen);
        if (status != 1 || finalLen != 0) {
            SetLastErrorV(UTP_ERR_DECRYPTION_ERROR, "EVP_DecryptInit_ex failed");
            return false;
        }

        decode(plaintext, outMeta);
        return true;
    };

    // 先用 key 验证, 失败再用 old key
    if (try_open_with_key(m_key)) {
        return true;
    }
    if (try_open_with_key(m_oldKey)) {
        return true;
    }

    return false;
}

void TokenAuth::onUpdateKey()
{
    TokenKey oldKey = m_key;
    if (RAND_bytes(m_key.data(), (int32_t)m_key.size()) != 1) {
        SetLastErrorV(UTP_ERR_RANDOM_GENERATION_FAILED, "RAND_bytes failed");
        return;
    }

    m_oldKey = oldKey;
}

void TokenAuth::encode(const TokenMeta &meta, std::array<uint8_t, TOKEN_META_SIZE> &plaintext)
{
    uint8_t *p = plaintext.data();

    auto put_u32 = [&] (uint32_t v) {
        v = htobe32(v);
        std::memcpy(p, &v, sizeof(v));
        p += sizeof(v);
    };

    put_u32(meta.timestamp);
    put_u32(meta.cid);
    put_u32(meta.version);
    put_u32(meta.secret);
}

void TokenAuth::decode(const std::array<uint8_t, TOKEN_META_SIZE> &plaintext, TokenMeta &meta)
{
    const uint8_t *p = plaintext.data();
    auto get_u32 = [&]() -> uint32_t {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        p += sizeof(v);
        return be32toh(v);
    };

    meta.timestamp = get_u32();
    meta.cid       = get_u32();
    meta.version   = get_u32();
    meta.secret    = get_u32();
}

} // namespace utp
} // namespace eular
