/*************************************************************************
    > File Name: resumption_state_codec.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Mar 2026 10:00:00 AM CST
 ************************************************************************/

#include "crypto/resumption_state_codec.h"

#include <array>
#include <string>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
constexpr uint8_t kResumptionStateMagic[4] = {'U', 'R', 'S', '1'};
constexpr size_t kResumptionNonceSize = 12;
constexpr size_t kResumptionTagSize = 16;
constexpr uint8_t kResumptionFormatVersion = 1;

const std::string kResumptionAad("UTP-SessionResumptionState-V1");
}

namespace eular {
namespace utp {

constexpr size_t ResumptionStateCodec::KEY_SIZE;

bool ResumptionStateCodec::Seal(const Key &key,
                                const std::vector<uint8_t> &plaintext,
                                std::vector<uint8_t> &sealed)
{
    sealed.clear();

    std::array<uint8_t, kResumptionNonceSize> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int32_t>(nonce.size())) != 1) {
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }

    bool ok = false;
    do {
        int32_t status = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        if (status != 1) {
            break;
        }

        status = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int32_t>(nonce.size()), nullptr);
        if (status != 1) {
            break;
        }

        status = EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data());
        if (status != 1) {
            break;
        }

        int32_t aadOutLen = 0;
        status = EVP_EncryptUpdate(ctx,
                                   nullptr,
                                   &aadOutLen,
                                   reinterpret_cast<const uint8_t *>(kResumptionAad.data()),
                                   static_cast<int32_t>(kResumptionAad.size()));
        if (status != 1) {
            break;
        }

        std::vector<uint8_t> ciphertext(plaintext.size() + kResumptionTagSize, 0);
        int32_t outLen = 0;
        status = EVP_EncryptUpdate(ctx,
                                   ciphertext.data(),
                                   &outLen,
                                   plaintext.data(),
                                   static_cast<int32_t>(plaintext.size()));
        if (status != 1) {
            break;
        }

        int32_t finalLen = 0;
        status = EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen, &finalLen);
        if (status != 1 || finalLen != 0) {
            break;
        }

        std::array<uint8_t, kResumptionTagSize> tag{};
        status = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int32_t>(tag.size()), tag.data());
        if (status != 1) {
            break;
        }

        ciphertext.resize(static_cast<size_t>(outLen));
        sealed.reserve(4 + 1 + nonce.size() + ciphertext.size() + tag.size());
        sealed.insert(sealed.end(), std::begin(kResumptionStateMagic), std::end(kResumptionStateMagic));
        sealed.push_back(kResumptionFormatVersion);
        sealed.insert(sealed.end(), nonce.begin(), nonce.end());
        sealed.insert(sealed.end(), ciphertext.begin(), ciphertext.end());
        sealed.insert(sealed.end(), tag.begin(), tag.end());
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool ResumptionStateCodec::Open(const Key &key,
                                const std::vector<uint8_t> &sealed,
                                std::vector<uint8_t> &plaintext)
{
    plaintext.clear();
    if (sealed.size() < 4 + 1 + kResumptionNonceSize + kResumptionTagSize) {
        return false;
    }

    if (std::memcmp(sealed.data(), kResumptionStateMagic, 4) != 0) {
        return false;
    }
    if (sealed[4] != kResumptionFormatVersion) {
        return false;
    }

    const uint8_t *nonce = sealed.data() + 5;
    const uint8_t *ciphertext = nonce + kResumptionNonceSize;
    const size_t ciphertextLen = sealed.size() - 5 - kResumptionNonceSize - kResumptionTagSize;
    const uint8_t *tag = sealed.data() + sealed.size() - kResumptionTagSize;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }

    bool ok = false;
    do {
        int32_t status = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        if (status != 1) {
            break;
        }

        status = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int32_t>(kResumptionNonceSize), nullptr);
        if (status != 1) {
            break;
        }

        status = EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);
        if (status != 1) {
            break;
        }

        int32_t aadOutLen = 0;
        status = EVP_DecryptUpdate(ctx,
                                   nullptr,
                                   &aadOutLen,
                                   reinterpret_cast<const uint8_t *>(kResumptionAad.data()),
                                   static_cast<int32_t>(kResumptionAad.size()));
        if (status != 1) {
            break;
        }

        plaintext.resize(ciphertextLen, 0);
        int32_t outLen = 0;
        status = EVP_DecryptUpdate(ctx,
                                   plaintext.data(),
                                   &outLen,
                                   ciphertext,
                                   static_cast<int32_t>(ciphertextLen));
        if (status != 1) {
            break;
        }

        status = EVP_CIPHER_CTX_ctrl(ctx,
                                     EVP_CTRL_AEAD_SET_TAG,
                                     static_cast<int32_t>(kResumptionTagSize),
                                     const_cast<uint8_t *>(tag));
        if (status != 1) {
            break;
        }

        int32_t finalLen = 0;
        status = EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen, &finalLen);
        if (status != 1 || finalLen != 0) {
            break;
        }

        plaintext.resize(static_cast<size_t>(outLen));
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

} // namespace utp
} // namespace eular