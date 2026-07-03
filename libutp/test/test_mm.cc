/*************************************************************************
    > File Name: test_mm.cc
    > Author: eular
    > Brief:
    > Created Time: Sat 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>
#include <cstring>

#include "proto/frame/version.h"
#include "proto/packet_in.h"
#include "proto/packet_out.h"
#include "util/mm.h"
#include <utils/serialize.hpp>

using eular::utp::FrameVersion;
using eular::utp::MemoryManager;
using eular::utp::PacketIn;
using eular::utp::PacketOut;
using eular::utp::Status;

TEST_CASE("MemoryManager: PacketIn allocates decode buffer", "[MemoryManager][PacketIn]")
{
    MemoryManager mm;
    PacketIn *packet = mm.getPacketIn(128);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->raw_data != nullptr);
    REQUIRE(packet->alloc_size >= 128);

    FrameVersion version;
    version.version = 1;

    std::array<uint8_t, 64> wire{};
    uint8_t *offset = wire.data();
    size_t left = wire.size();
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint32_t>(1));
    REQUIRE(offset != nullptr);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint32_t>(2));
    REQUIRE(offset != nullptr);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint64_t>(3));
    REQUIRE(offset != nullptr);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint16_t>(FRAME_VERSION_SIZE));
    REQUIRE(offset != nullptr);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint8_t>(UTP_TYPE_INITIAL));
    REQUIRE(offset != nullptr);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);
    Status st;
    REQUIRE(version.encode(offset, left, st) == FRAME_VERSION_SIZE);

    const size_t wireSize = UTP_HEADER_SIZE + FRAME_VERSION_SIZE;
    std::memcpy(const_cast<uint8_t *>(packet->raw_data), wire.data(), wireSize);
    packet->raw_size = wireSize;

    REQUIRE(packet->decode(packet->raw_data, packet->raw_size).ok());
    REQUIRE(packet->header.scid == 1);
    REQUIRE(packet->header.dcid == 2);
    REQUIRE(packet->hasFrame(eular::utp::kFrameVersion));

    const uint8_t *raw = packet->raw_data;
    mm.putPacketIn(packet);

    PacketIn *reused = mm.getPacketIn(64);
    REQUIRE(reused != nullptr);
    REQUIRE(reused->raw_data == raw);
    mm.putPacketIn(reused);
}

TEST_CASE("MemoryManager: putPacketIn duplicate release is idempotent", "[MemoryManager][PacketIn]")
{
    MemoryManager mm;
    PacketIn *packet = mm.getPacketIn(128);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->raw_data != nullptr);

    mm.putPacketIn(packet);
    // Releasing the same object again should be a no-op instead of corrupting pools.
    mm.putPacketIn(packet);

    PacketIn *reused = mm.getPacketIn(128);
    REQUIRE(reused != nullptr);
    REQUIRE(reused->raw_data != nullptr);
    mm.putPacketIn(reused);
}

TEST_CASE("MemoryManager: putPacketOut duplicate release is idempotent", "[MemoryManager][PacketOut]")
{
    MemoryManager mm;
    PacketOut *packet = mm.getPacketOut(256);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->raw_data != nullptr);

    mm.putPacketOut(packet);
    // Releasing the same object again should be a no-op instead of corrupting pools.
    mm.putPacketOut(packet);

    PacketOut *reused = mm.getPacketOut(256);
    REQUIRE(reused != nullptr);
    REQUIRE(reused->raw_data != nullptr);
    mm.putPacketOut(reused);
}
