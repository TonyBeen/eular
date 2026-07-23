/*************************************************************************
    > File Name: test_frame_ack_decode.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>

#include <utils/serialize.hpp>

#include "proto/frame/ack.h"

using eular::Serialize;
using eular::utp::AckInfo;
using eular::utp::FrameAck;
using eular::utp::FrameType;
using eular::utp::Status;
using eular::utp::TransportParams;

TEST_CASE("Ack frame decode sets largest ack and ranges", "[FrameAck]")
{
    // 构造一个 ACK:
    // largest=100, first range length=6 => [95,100]
    // range_count=1, gap=2, len=3 => [90,92]
    std::array<uint8_t, 64> bytes{};
    uint8_t *offset = bytes.data();
    size_t left = bytes.size();

    offset = Serialize::SerializeTo(offset, left, FrameType::kFrameAck);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(1)); // range_count
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(5)); // ack_delay encoded
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(6)); // first range len
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint64_t>(100)); // largest
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(2)); // gap
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(3)); // second len
    REQUIRE(offset != nullptr);

    const size_t frameLen = bytes.size() - left;

    TransportParams params;
    params.ack_delay_exponent = 3;

    AckInfo ackInfo;
    ackInfo.reset();

    FrameAck ack;
    ack._ackInfo = &ackInfo;
    ack._params = &params;

    Status st;
    int32_t decoded = ack.decode(bytes.data(), frameLen, st);
    REQUIRE(st.ok());
    REQUIRE(decoded == static_cast<int32_t>(frameLen));

    REQUIRE(ackInfo.largest_ack_packno == 100);
    REQUIRE(ackInfo.range_size == 2);

    REQUIRE(ackInfo.ack_ranges[0].low == 95);
    REQUIRE(ackInfo.ack_ranges[0].high == 100);

    REQUIRE(ackInfo.ack_ranges[1].low == 90);
    REQUIRE(ackInfo.ack_ranges[1].high == 92);

    REQUIRE(ackInfo.ack_delay == (static_cast<utp_time_t>(5) << params.ack_delay_exponent));
}

TEST_CASE("Ack frame rejects unsafe exponent and invalid encode pointers", "[FrameAck][Regression]")
{
    std::array<uint8_t, FRAME_ACK_HDR_SIZE> bytes{};
    TransportParams params;
    params.ack_delay_exponent = TransportParams::kMaxAckDelayExponent + 1;
    AckInfo ackInfo;

    FrameAck decoder;
    decoder._ackInfo = &ackInfo;
    decoder._params = &params;
    Status decodeStatus;
    REQUIRE(decoder.decode(bytes.data(), bytes.size(), decodeStatus) < 0);
    REQUIRE(decodeStatus.code() == UTP_ERR_INVALID_PARAM);

    FrameAck encoder;
    Status encodeStatus;
    REQUIRE(encoder.encode(nullptr, 0, encodeStatus) < 0);
    REQUIRE(encodeStatus.code() == UTP_ERR_INVALID_PARAM);
}
