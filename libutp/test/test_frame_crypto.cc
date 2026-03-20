/*************************************************************************
    > File Name: test_frame_crypto.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include "proto/frame/crypto.h"

using eular::utp::FrameCrypto;
using eular::utp::FrameCryptoType;
using eular::utp::TransportParams;

TEST_CASE("Crypto frame: encode/decode", "[FrameCrypto]")
{
    TransportParams localTp;
    localTp.flags = TransportParams::kDefaultFlags;
    localTp.max_idle_timeout = 12345;
    localTp.handshake_timeout = 2345;
    localTp.init_max_streams_bidi = 48;
    localTp.init_max_streams_uni = 24;
    localTp.ack_delay_exponent = 4;
    localTp.max_ack_delta = 12;
    localTp.max_ack_delay = 66;

    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> localPubkey{};
    for (size_t i = 0; i < localPubkey.size(); ++i) {
        localPubkey[i] = static_cast<uint8_t>(i + 1);
    }

    FrameCrypto frame;
    frame.crypto_type = FrameCryptoType::kFrameCryptoAESGCM128;
    frame.tp_size = static_cast<uint8_t>(TransportParams::kMaxNumeric);
    frame.tp = &localTp;
    frame.eph_pubkey = localPubkey.data();

    std::array<uint8_t, 256> buffer{};
    int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_CRYPTO_SIZE);

    TransportParams peerTp;
    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubkey{};
    FrameCrypto decoded;
    decoded.tp = &peerTp;
    decoded.eph_pubkey = peerPubkey.data();

    int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.crypto_type == FrameCryptoType::kFrameCryptoAESGCM128);

    REQUIRE(peerTp.flags == localTp.flags);
    REQUIRE(peerTp.max_idle_timeout == localTp.max_idle_timeout);
    REQUIRE(peerTp.handshake_timeout == localTp.handshake_timeout);
    REQUIRE(peerTp.init_max_streams_bidi == localTp.init_max_streams_bidi);
    REQUIRE(peerTp.init_max_streams_uni == localTp.init_max_streams_uni);
    REQUIRE(peerTp.ack_delay_exponent == localTp.ack_delay_exponent);
    REQUIRE(peerTp.max_ack_delta == localTp.max_ack_delta);
    REQUIRE(peerTp.max_ack_delay == localTp.max_ack_delay);

    REQUIRE(peerPubkey == localPubkey);
}
