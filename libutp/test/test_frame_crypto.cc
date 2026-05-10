/*************************************************************************
    > File Name: test_frame_crypto.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>

#include "proto/frame/crypto.h"

using eular::utp::FrameCrypto;
using eular::utp::FrameCryptoType;
using eular::utp::Status;

TEST_CASE("Crypto frame: encode/decode", "[FrameCrypto]")
{
    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> localPubkey{};
    for (size_t i = 0; i < localPubkey.size(); ++i) {
        localPubkey[i] = static_cast<uint8_t>(i + 1);
    }

    FrameCrypto frame;
    frame.crypto_type = FrameCryptoType::kFrameCryptoAESGCM128;
    frame.eph_pubkey = localPubkey.data();

    std::array<uint8_t, 256> buffer{};
    Status st;
    int32_t encoded = frame.encode(buffer.data(), buffer.size(), st);
    REQUIRE(st.ok());
    REQUIRE(encoded == FRAME_CRYPTO_SIZE);

    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubkey{};
    FrameCrypto decoded;
    decoded.eph_pubkey = peerPubkey.data();

    int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded), st);
    REQUIRE(st.ok());
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.crypto_type == FrameCryptoType::kFrameCryptoAESGCM128);

    REQUIRE(peerPubkey == localPubkey);
}
