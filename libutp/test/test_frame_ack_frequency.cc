/*************************************************************************
    > File Name: test_frame_ack_frequency.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include <utils/serialize.hpp>

#include "proto/frame/ack_frequency.h"

using eular::Serialize;
using eular::utp::FrameAckFrequency;
using eular::utp::FrameType;

TEST_CASE("AckFrequency decode clamps zero fields to defaults", "[FrameAckFrequency]")
{
    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> bytes{};
    uint8_t *offset = bytes.data();
    size_t left = bytes.size();

    offset = Serialize::SerializeTo(offset, left, FrameType::kFrameAckFrequency);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(0));
    REQUIRE(offset != nullptr);

    FrameAckFrequency ackFreq;
    const int32_t decoded = ackFreq.decode(bytes.data(), bytes.size());
    REQUIRE(decoded == FRAME_ACK_FREQUENCY_SIZE);
    REQUIRE(ackFreq.ack_eliciting_threshold == FrameAckFrequency::kDefaultAckElicitingThreshold);
    REQUIRE(ackFreq.reordering_threshold == FrameAckFrequency::kDefaultReorderingThreshold);
    REQUIRE(ackFreq.max_ack_delay_ms == FrameAckFrequency::kDefaultMaxAckDelayMs);
}

TEST_CASE("AckFrequency decode clamps oversized fields", "[FrameAckFrequency]")
{
    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> bytes{};
    uint8_t *offset = bytes.data();
    size_t left = bytes.size();

    offset = Serialize::SerializeTo(offset, left, FrameType::kFrameAckFrequency);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(255));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(255));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint32_t>(5000));
    REQUIRE(offset != nullptr);

    FrameAckFrequency ackFreq;
    const int32_t decoded = ackFreq.decode(bytes.data(), bytes.size());
    REQUIRE(decoded == FRAME_ACK_FREQUENCY_SIZE);
    REQUIRE(ackFreq.ack_eliciting_threshold == FrameAckFrequency::kMaxAckElicitingThreshold);
    REQUIRE(ackFreq.reordering_threshold == FrameAckFrequency::kMaxReorderingThreshold);
    REQUIRE(ackFreq.max_ack_delay_ms == FrameAckFrequency::kMaxAckDelayMsClamp);
}

TEST_CASE("AckFrequency encode normalizes invalid values", "[FrameAckFrequency]")
{
    FrameAckFrequency ackFreq;
    ackFreq.ack_eliciting_threshold = 0;
    ackFreq.reordering_threshold = 0;
    ackFreq.max_ack_delay_ms = 0;

    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> bytes{};
    const int32_t encoded = ackFreq.encode(bytes.data(), bytes.size());
    REQUIRE(encoded == FRAME_ACK_FREQUENCY_SIZE);

    FrameAckFrequency decoded;
    REQUIRE(decoded.decode(bytes.data(), bytes.size()) == FRAME_ACK_FREQUENCY_SIZE);
    REQUIRE(decoded.ack_eliciting_threshold == FrameAckFrequency::kDefaultAckElicitingThreshold);
    REQUIRE(decoded.reordering_threshold == FrameAckFrequency::kDefaultReorderingThreshold);
    REQUIRE(decoded.max_ack_delay_ms == FrameAckFrequency::kDefaultMaxAckDelayMs);
}
