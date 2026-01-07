/*************************************************************************
    > File Name: x25519_wrapper.h
    > Author: eular
    > Brief:
    > Created Time: Tue 30 Dec 2025 10:59:35 AM CST
 ************************************************************************/

#ifndef __CRYPTO_X25519_WRAPPER_H__
#define __CRYPTO_X25519_WRAPPER_H__

#include <string.h>
#include <array>

#include <openssl/curve25519.h>
#include <openssl/rand.h>
#include <openssl/mem.h>

namespace eular {
namespace utp {
class X25519Wrapper
{
    X25519Wrapper(const X25519Wrapper&) = delete;
    X25519Wrapper& operator=(const X25519Wrapper&) = delete;

public:
    static constexpr size_t PRIVATE_KEY_SIZE = X25519_PRIVATE_KEY_LEN;  // 32
    static constexpr size_t PUBLIC_KEY_SIZE = X25519_PUBLIC_VALUE_LEN;  // 32
    static constexpr size_t SHARED_SECRET_SIZE = X25519_SHARED_KEY_LEN; // 32

    using PrivateKey = std::array<uint8_t, PRIVATE_KEY_SIZE>;
    using PublicKey = std::array<uint8_t, PUBLIC_KEY_SIZE>;
    using SharedSecret = std::array<uint8_t, SHARED_SECRET_SIZE>;
    using SharedSecretShort = std::array<uint8_t, SHARED_SECRET_SIZE / 2>;

    X25519Wrapper();
    explicit X25519Wrapper(const PrivateKey& private_key);
    ~X25519Wrapper();

    X25519Wrapper(X25519Wrapper&&) = default;
    X25519Wrapper& operator=(X25519Wrapper&&) = default;

    const PublicKey&    publicKey() const { return m_publicKey; }
    SharedSecret        deriveSharedSecret(const PublicKey& peerPublicKey) const;
    SharedSecretShort   deriveSharedSecretShort(const PublicKey& peerPublicKey) const;

private:
    void secureZero(void* ptr, size_t len) {
        // NOTE 不使用memset的原因是编译器可能会优化掉对不再使用的内存的清零操作.
        // 使用 volatile 指针防止编译器优化掉清零操作
        volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
        while (len--) {
            *p++ = 0;
        }
    }

private:
    PrivateKey  m_privateKey;
    PublicKey   m_publicKey;
};

} // namespace utp
} // namespace eular

#endif // __CRYPTO_X25519_WRAPPER_H__
