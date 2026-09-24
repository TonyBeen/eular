/*************************************************************************
    > File Name: test_traffic_key_schedule.cc
 ************************************************************************/

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>

#include "crypto/aes_gcm_context.h"
#include "crypto/traffic_key_schedule.h"
#include "crypto/x25519_wrapper.h"
#include "proto/frame/crypto.h"

using eular::utp::AesGcmContext;
using eular::utp::FrameCryptoType;
using eular::utp::TrafficKeySchedule;
using eular::utp::X25519Wrapper;

TEST_CASE("Traffic keys: HKDF derives matching direction-separated contexts", "[Crypto][HKDF]")
{
    X25519Wrapper client;
    X25519Wrapper server;

    std::shared_ptr<AesGcmContext> clientTx;
    std::shared_ptr<AesGcmContext> clientRx;
    std::shared_ptr<AesGcmContext> serverTx;
    std::shared_ptr<AesGcmContext> serverRx;
    const uint8_t cryptoType = static_cast<uint8_t>(FrameCryptoType::kFrameCryptoAESGCM256);

    REQUIRE(TrafficKeySchedule::CreateAesGcmContexts(client, server.publicKey(), 1001, 2001, true, cryptoType, 32,
                                                     clientTx, clientRx).ok());
    REQUIRE(TrafficKeySchedule::CreateAesGcmContexts(server, client.publicKey(), 2001, 1001, false, cryptoType, 32,
                                                     serverTx, serverRx).ok());

    const std::array<uint8_t, 8> plaintext{{1, 2, 3, 4, 5, 6, 7, 8}};
    const std::array<uint8_t, 20> aad{{0}};
    std::array<uint8_t, 32> clientCipher{};
    std::array<uint8_t, 32> serverCipher{};
    size_t clientCipherLen = clientCipher.size();
    size_t serverCipherLen = serverCipher.size();

    REQUIRE(clientTx->encrypt(plaintext.data(), plaintext.size(), aad.data(), aad.size(), 7, clientCipher.data(),
                              &clientCipherLen).ok());
    REQUIRE(serverTx->encrypt(plaintext.data(), plaintext.size(), aad.data(), aad.size(), 7, serverCipher.data(),
                              &serverCipherLen).ok());
    REQUIRE(clientCipherLen == plaintext.size() + AesGcmContext::GCM_TAG_SIZE);
    REQUIRE(serverCipherLen == clientCipherLen);
    REQUIRE_FALSE(std::equal(clientCipher.begin(), clientCipher.begin() + clientCipherLen, serverCipher.begin()));

    std::array<uint8_t, 16> decoded{};
    size_t decodedLen = decoded.size();
    REQUIRE(serverRx->decrypt(clientCipher.data(), clientCipherLen, aad.data(), aad.size(), 7, decoded.data(),
                              &decodedLen).ok());
    REQUIRE(decodedLen == plaintext.size());
    REQUIRE(std::equal(plaintext.begin(), plaintext.end(), decoded.begin()));

    decoded.fill(0);
    decodedLen = decoded.size();
    REQUIRE(clientRx->decrypt(serverCipher.data(), serverCipherLen, aad.data(), aad.size(), 7, decoded.data(),
                              &decodedLen).ok());
    REQUIRE(std::equal(plaintext.begin(), plaintext.end(), decoded.begin()));

    decodedLen = decoded.size();
    REQUIRE_FALSE(clientRx->decrypt(clientCipher.data(), clientCipherLen, aad.data(), aad.size(), 7, decoded.data(),
                                    &decodedLen).ok());
}

TEST_CASE("Traffic keys: transcript changes alter derived material", "[Crypto][HKDF]")
{
    X25519Wrapper client;
    X25519Wrapper server;
    const X25519Wrapper::SharedSecret shared = client.deriveSharedSecret(server.publicKey());

    TrafficKeySchedule::Material first;
    TrafficKeySchedule::Material changed;
    const uint8_t cryptoType = static_cast<uint8_t>(FrameCryptoType::kFrameCryptoAESGCM128);
    REQUIRE(TrafficKeySchedule::Derive(shared, client.publicKey(), server.publicKey(), 10, 20, cryptoType, 16,
                                       first).ok());
    REQUIRE(TrafficKeySchedule::Derive(shared, client.publicKey(), server.publicKey(), 10, 21, cryptoType, 16,
                                       changed).ok());
    REQUIRE_FALSE(std::equal(first.clientToServer.key.begin(), first.clientToServer.key.begin() + first.keySize,
                             changed.clientToServer.key.begin()));
    REQUIRE_FALSE(std::equal(first.clientToServer.key.begin(), first.clientToServer.key.begin() + first.keySize,
                             first.serverToClient.key.begin()));
    REQUIRE(first.clientToServer.noncePrefix != first.serverToClient.noncePrefix);
}

TEST_CASE("Traffic keys: invalid X25519 inputs are rejected", "[Crypto][HKDF]")
{
    X25519Wrapper client;
    X25519Wrapper::PublicKey zeroPublic{};
    REQUIRE_THROWS(client.deriveSharedSecret(zeroPublic));

    X25519Wrapper::SharedSecret zeroShared{};
    TrafficKeySchedule::Material material;
    REQUIRE_FALSE(TrafficKeySchedule::Derive(zeroShared, client.publicKey(), zeroPublic, 1, 2,
                                             static_cast<uint8_t>(FrameCryptoType::kFrameCryptoAESGCM256), 32,
                                             material).ok());
}
