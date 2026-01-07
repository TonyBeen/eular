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

namespace eular {
namespace utp {
X25519Wrapper::X25519Wrapper()
{
    X25519_keypair(m_publicKey.data(), m_privateKey.data());
}

X25519Wrapper::X25519Wrapper(const PrivateKey &private_key) :
    m_privateKey(private_key)
{
    X25519_public_from_private(m_publicKey.data(), m_privateKey.data());
}

X25519Wrapper::~X25519Wrapper()
{
    secureZero(m_privateKey.data(), m_privateKey.size());
}

X25519Wrapper::SharedSecret X25519Wrapper::deriveSharedSecret(const PublicKey &peerPublicKey) const
{
    SharedSecret sharedSecret;
    if (!X25519(sharedSecret.data(), m_privateKey.data(), peerPublicKey.data())) {
        throw std::runtime_error("X25519 Key exchange failed.");
    }

    return sharedSecret;
}

X25519Wrapper::SharedSecretShort X25519Wrapper::deriveSharedSecretShort(const PublicKey &peerPublicKey) const
{
    SharedSecret sharedSecret;
    if (!X25519(sharedSecret.data(), m_privateKey.data(), peerPublicKey.data())) {
        throw std::runtime_error("X25519 Key exchange failed.");
    }

    SharedSecretShort sharedSecretShort;
    std::copy_n(sharedSecret.data(), sharedSecretShort.size(), sharedSecretShort.data());
    return sharedSecretShort;
}

} // namespace utp
} // namespace eular
