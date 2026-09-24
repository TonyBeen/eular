/*************************************************************************
    > File Name: x25519_wrapper.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 30 Dec 2025 10:59:41 AM CST
 ************************************************************************/

#include "crypto/x25519_wrapper.h"

#include <string.h>
#include <stdexcept>
#include <algorithm>

#include <openssl/mem.h>

namespace eular {
namespace utp {

#if !UTP_HAVE_OPENSSL_CURVE25519
namespace {

X25519Wrapper::PublicKey PublicFromPrivate(const X25519Wrapper::PrivateKey &privateKey)
{
    X25519Wrapper::PublicKey publicKey{};

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,
                                                   nullptr,
                                                   privateKey.data(),
                                                   privateKey.size());
    if (pkey == nullptr) {
        throw std::runtime_error("X25519 create private key failed");
    }

    size_t publicLen = publicKey.size();
    int32_t status = EVP_PKEY_get_raw_public_key(pkey, publicKey.data(), &publicLen);
    EVP_PKEY_free(pkey);
    if (status != 1 || publicLen != publicKey.size()) {
        throw std::runtime_error("X25519 derive public key failed");
    }

    return publicKey;
}

} // namespace
#endif

X25519Wrapper::X25519Wrapper()
{
#if UTP_HAVE_OPENSSL_CURVE25519
    X25519_keypair(m_publicKey.data(), m_privateKey.data());
#else
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (ctx == nullptr) {
        throw std::runtime_error("X25519 keygen context create failed");
    }

    EVP_PKEY *pkey = nullptr;
    int32_t status = EVP_PKEY_keygen_init(ctx);
    status = (status == 1) ? EVP_PKEY_keygen(ctx, &pkey) : 0;
    EVP_PKEY_CTX_free(ctx);

    if (status != 1 || pkey == nullptr) {
        if (pkey != nullptr) {
            EVP_PKEY_free(pkey);
        }
        throw std::runtime_error("X25519 keygen failed");
    }

    size_t privateLen = m_privateKey.size();
    size_t publicLen = m_publicKey.size();
    status = EVP_PKEY_get_raw_private_key(pkey, m_privateKey.data(), &privateLen);
    status = (status == 1) ? EVP_PKEY_get_raw_public_key(pkey, m_publicKey.data(), &publicLen) : 0;
    EVP_PKEY_free(pkey);

    if (status != 1 || privateLen != m_privateKey.size() || publicLen != m_publicKey.size()) {
        throw std::runtime_error("X25519 export raw key failed");
    }
#endif
}

X25519Wrapper::X25519Wrapper(const PrivateKey &private_key) :
    m_privateKey(private_key)
{
#if UTP_HAVE_OPENSSL_CURVE25519
    X25519_public_from_private(m_publicKey.data(), m_privateKey.data());
#else
    m_publicKey = PublicFromPrivate(m_privateKey);
#endif
}

X25519Wrapper::~X25519Wrapper()
{
    secureZero(m_privateKey.data(), m_privateKey.size());
}

X25519Wrapper::SharedSecret X25519Wrapper::deriveSharedSecret(const PublicKey &peerPublicKey) const
{
    SharedSecret sharedSecret{};
#if UTP_HAVE_OPENSSL_CURVE25519
    if (!X25519(sharedSecret.data(), m_privateKey.data(), peerPublicKey.data())) {
        throw std::runtime_error("X25519 Key exchange failed.");
    }
#else
    EVP_PKEY *privateKey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,
                                                         nullptr,
                                                         m_privateKey.data(),
                                                         m_privateKey.size());
    EVP_PKEY *peerKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519,
                                                     nullptr,
                                                     peerPublicKey.data(),
                                                     peerPublicKey.size());
    if (privateKey == nullptr || peerKey == nullptr) {
        if (privateKey != nullptr) {
            EVP_PKEY_free(privateKey);
        }
        if (peerKey != nullptr) {
            EVP_PKEY_free(peerKey);
        }
        throw std::runtime_error("X25519 key create failed");
    }

    EVP_PKEY_CTX *deriveCtx = EVP_PKEY_CTX_new(privateKey, nullptr);
    if (deriveCtx == nullptr) {
        EVP_PKEY_free(privateKey);
        EVP_PKEY_free(peerKey);
        throw std::runtime_error("X25519 derive context create failed");
    }

    int32_t status = EVP_PKEY_derive_init(deriveCtx);
    status = (status == 1) ? EVP_PKEY_derive_set_peer(deriveCtx, peerKey) : 0;
    size_t outLen = sharedSecret.size();
    status = (status == 1) ? EVP_PKEY_derive(deriveCtx, sharedSecret.data(), &outLen) : 0;

    EVP_PKEY_CTX_free(deriveCtx);
    EVP_PKEY_free(privateKey);
    EVP_PKEY_free(peerKey);

    if (status != 1 || outLen != sharedSecret.size()) {
        throw std::runtime_error("X25519 key exchange failed");
    }
#endif

    uint8_t aggregate = 0;
    for (uint8_t byte : sharedSecret) {
        aggregate |= byte;
    }
    if (aggregate == 0) {
        throw std::runtime_error("X25519 produced an all-zero shared secret");
    }

    return sharedSecret;
}

X25519Wrapper::SharedSecretShort X25519Wrapper::deriveSharedSecretShort(const PublicKey &peerPublicKey) const
{
    SharedSecret sharedSecret = deriveSharedSecret(peerPublicKey);

    SharedSecretShort sharedSecretShort;
    std::copy_n(sharedSecret.data(), sharedSecretShort.size(), sharedSecretShort.data());
    OPENSSL_cleanse(sharedSecret.data(), sharedSecret.size());
    return sharedSecretShort;
}

} // namespace utp
} // namespace eular
