/*************************************************************************
    > File Name: token.h
    > Author: eular
    > Brief:
    > Created Time: Tue 03 Feb 2026 04:27:24 PM CST
 ************************************************************************/

#ifndef __UTP_CRYPTO_TOKEN_H__
#define __UTP_CRYPTO_TOKEN_H__

#include <stdint.h>
#include <array>
#include <vector>
#include <string>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <event/timer.h>

#define AEAD_KEY_SIZE    32
#define AEAD_NONCE_SIZE  12
#define AEAD_TAG_SIZE    16

namespace eular {
namespace utp {

struct TokenMeta {
    uint32_t    timestamp;  // unix seconds
    uint32_t    cid;        // connection id
    uint32_t    version;    // peer utp version
    uint32_t    secret;     // local secret number
};

static const size_t TOKEN_META_SIZE = sizeof(TokenMeta);
static const std::string TOKEN_AAD("UTP-Token-AAD");

// Token format: nonce(12) || ciphertext(sizeof(TokenMeta)) || tag(16)
static const size_t TOKEN_SIZE = AEAD_NONCE_SIZE + TOKEN_META_SIZE + AEAD_TAG_SIZE;

class TokenAuth
{
    TokenAuth(const TokenAuth &) = delete;
    TokenAuth &operator=(const TokenAuth &) = delete;
public:
    using TokenKey = std::array<uint8_t, AEAD_KEY_SIZE>;
    using TokenBuf = std::array<uint8_t, TOKEN_SIZE>;

    TokenAuth(event_base *base);
    ~TokenAuth();

    bool seal(const TokenMeta &meta, TokenBuf &outToken);
    bool open(const TokenBuf &token, TokenMeta &outMeta);

private:
    void onUpdateKey();
    void encode(const TokenMeta &meta, std::array<uint8_t, TOKEN_META_SIZE> &plaintext);
    void decode(const std::array<uint8_t, TOKEN_META_SIZE> &plaintext, TokenMeta &meta);

private:
    EVP_CIPHER_CTX* m_sealCtx{nullptr};
    EVP_CIPHER_CTX* m_openCtx{nullptr};
    TokenMeta       m_meta;
    TokenKey        m_key;
    TokenKey        m_oldKey;
    ev::EventTimer  m_timerUpdateKey;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_TOKEN_H__
