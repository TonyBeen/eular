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
#include <cstring>
#include <vector>
#include <string>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <event/timer.h>

#include "utp/platform.h"

#if defined(OS_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#elif defined(OS_LINUX) || defined(OS_APPLE)
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netdb.h>
#endif


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
    uint16_t    family;     // peer address family
    union {
        in_addr     v4;     // peer ipv4 address
        in6_addr    v6;     // peer ip address
    } host;
#define host_v4 host.v4
#define host_v6 host.v6

    TokenMeta() {
        std::memset(&this->host, 0, sizeof(this->host));
    }
};

static const size_t TOKEN_META_SIZE = 4 + 4 + 4 + 4 + 2 + 16; // 34 bytes
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
