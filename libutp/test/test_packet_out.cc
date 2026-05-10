/*************************************************************************
    > File Name: test_packet_out.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <cstdint>
#include <limits>

#include "proto/packet_out.h"
#include "util/mm.h"

using eular::utp::kFrameVersion;
using eular::utp::MemoryManager;
using eular::utp::PacketOut;
using eular::utp::Status;

TEST_CASE("PacketOut: create and destroy", "[PacketOut]")
{
    MemoryManager mm;
    PacketOut *pkt = mm.getPacketOut(128);
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->raw_data != nullptr);
    REQUIRE(pkt->alloc_size >= UTP_HEADER_SIZE);

    pkt->packno = 123;
    pkt->data_size = 64;
    pkt->frame_types = (1u << static_cast<uint32_t>(kFrameVersion));

    mm.putPacketOut(pkt);
}

TEST_CASE("PacketOut: reset keeps allocation", "[PacketOut]")
{
    MemoryManager mm;
    PacketOut *pkt = mm.getPacketOut(256);
    REQUIRE(pkt != nullptr);

    uint8_t *raw = pkt->raw_data;
    uint16_t allocSize = pkt->alloc_size;

    pkt->packno = 99;
    pkt->data_size = 80;
    pkt->frame_types = 0xFFFF;
    pkt->po_flags = 0x1234;

    pkt->reset();

    REQUIRE(pkt->raw_data == raw);
    REQUIRE(pkt->alloc_size == allocSize);
    REQUIRE(pkt->packno == 0);
    REQUIRE(pkt->data_size == 0);
    REQUIRE(pkt->frame_types == 0);
    REQUIRE(pkt->po_flags == 0);
    REQUIRE(pkt->loss_chain == pkt);

    mm.putPacketOut(pkt);
}

TEST_CASE("PacketOut: large requested allocation is clamped by bucket", "[PacketOut]")
{
    MemoryManager mm;
    PacketOut *pkt = mm.getPacketOut(
        static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1u);
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->alloc_size == std::numeric_limits<uint16_t>::max());
    mm.putPacketOut(pkt);
}
