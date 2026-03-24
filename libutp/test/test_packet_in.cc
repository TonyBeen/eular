/*************************************************************************
    > File Name: test_packet_in.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>
#include <cstring>

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "proto/packet_in.h"
#include "proto/frame/version.h"
#include "proto/frame/path.h"

using eular::Serialize;
using eular::utp::FramePathChallenge;
using eular::utp::FrameType;
using eular::utp::FrameVersion;
using eular::utp::PacketIn;

TEST_CASE("PacketIn: decode and iterate frames", "[PacketIn]")
{
    FrameVersion version;
    version.version = 1;

    FramePathChallenge challenge;
    challenge.data = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};

    std::array<uint8_t, 256> payload{};
    uint8_t *payloadOffset = payload.data();
    int32_t versionLen = version.encode(payloadOffset, payload.size());
    REQUIRE(versionLen > 0);
    payloadOffset += versionLen;

    int32_t challengeLen = challenge.encode(payloadOffset,
                                            payload.size() - static_cast<size_t>(versionLen));
    REQUIRE(challengeLen > 0);
    payloadOffset += challengeLen;

    const uint16_t payloadLen = static_cast<uint16_t>(versionLen + challengeLen);

    std::array<uint8_t, 512> packetBytes{};
    uint8_t *offset = packetBytes.data();
    size_t left = packetBytes.size();

    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(1001));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(2002));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint64_t>(7));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, payloadLen);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(UTP_TYPE_CTRL));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);

    std::memcpy(offset, payload.data(), payloadLen);
    const size_t packetSize = UTP_HEADER_SIZE + payloadLen;

    PacketIn packet;
    REQUIRE(packet.decode(packetBytes.data(), packetSize) == UTP_ERR_OK);
    REQUIRE(packet.header.scid == 1001);
    REQUIRE(packet.header.dcid == 2002);
    REQUIRE(packet.header.pn == 7);
    REQUIRE(packet.payload_size == payloadLen);

    REQUIRE(packet.hasFrame(FrameType::kFrameVersion));
    REQUIRE(packet.hasFrame(FrameType::kFramePathChallenge));
    REQUIRE_FALSE(packet.hasFrame(FrameType::kFrameSessionToken));

    size_t frameOffset = 0;
    FrameType frameType = FrameType::kFrameInvalid;
    const uint8_t *frameData = nullptr;
    size_t frameLen = 0;

    REQUIRE(packet.nextFrame(frameOffset, frameType, frameData, frameLen) > 0);
    REQUIRE(frameType == FrameType::kFrameVersion);
    REQUIRE(frameLen == FRAME_VERSION_SIZE);

    REQUIRE(packet.nextFrame(frameOffset, frameType, frameData, frameLen) > 0);
    REQUIRE(frameType == FrameType::kFramePathChallenge);
    REQUIRE(frameLen == FRAME_PATH_FRAME_SIZE);

    REQUIRE(packet.nextFrame(frameOffset, frameType, frameData, frameLen) < 0);
}

TEST_CASE("PacketIn: reject truncated payload", "[PacketIn]")
{
    std::array<uint8_t, UTP_HEADER_SIZE + 2> bytes{};
    uint8_t *offset = bytes.data();
    size_t left = bytes.size();

    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(1));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(2));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint64_t>(3));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(8)); // larger than actual payload
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(UTP_TYPE_CTRL));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);

    offset[0] = static_cast<uint8_t>(FrameType::kFramePing);
    offset[1] = static_cast<uint8_t>(FrameType::kFramePing);

    PacketIn packet;
    REQUIRE(packet.decode(bytes.data(), bytes.size()) < 0);
}
