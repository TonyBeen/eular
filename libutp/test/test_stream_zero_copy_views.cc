/*************************************************************************
    > File Name: test_stream_zero_copy_views.cc
    > Author: copilot
    > Brief:
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>
#include <cstring>

#include "utp/errno.h"
#include "proto/packet_in.h"

#define private public
#include "context/stream_impl.h"
#undef private

using eular::utp::FrameStream;
using eular::utp::Stream;
using eular::utp::StreamImpl;

TEST_CASE("StreamImpl: acquireReadViews supports out-of-order reassembly", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 31);

    std::array<uint8_t, 2> tail = {3, 4};
    FrameStream f1;
    f1.stream_id = 31;
    f1.stream_offset = 2;
    f1.stream_data_length = static_cast<uint16_t>(tail.size());
    f1.stream_data = tail.data();
    REQUIRE(stream.onFrame(f1) == UTP_ERR_OK);
    REQUIRE_FALSE(stream.readable());

    std::array<uint8_t, 2> head = {1, 2};
    FrameStream f0;
    f0.stream_id = 31;
    f0.stream_offset = 0;
    f0.stream_data_length = static_cast<uint16_t>(head.size());
    f0.stream_data = head.data();
    REQUIRE(stream.onFrame(f0) == UTP_ERR_OK);
    REQUIRE(stream.readable());

    Stream::ConstBufferView views[2];
    const size_t bytes = stream.acquireReadViews(views, 8);
    REQUIRE(bytes == 4);
    REQUIRE(views[0].data != nullptr);
    REQUIRE(views[0].len == 2);
    REQUIRE(std::memcmp(views[0].data, head.data(), 2) == 0);
    REQUIRE(views[1].data != nullptr);
    REQUIRE(views[1].len == 2);
    REQUIRE(std::memcmp(views[1].data, tail.data(), 2) == 0);

    REQUIRE(stream.commitReadViews(3) == 3);

    Stream::ConstBufferView after[2];
    const size_t left = stream.acquireReadViews(after, 8);
    REQUIRE(left == 1);
    REQUIRE(after[0].len == 1);
    REQUIRE(*static_cast<const uint8_t *>(after[0].data) == 4);

    REQUIRE(stream.commitReadViews(1) == 1);
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: read and read-views stay consistent", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 32);

    std::array<uint8_t, 5> payload = {9, 8, 7, 6, 5};
    FrameStream frame;
    frame.stream_id = 32;
    frame.stream_offset = 0;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();
    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);

    Stream::ConstBufferView views[2];
    REQUIRE(stream.acquireReadViews(views, 2) == 2);
    REQUIRE(views[0].len == 2);
    REQUIRE(std::memcmp(views[0].data, payload.data(), 2) == 0);
    REQUIRE(stream.commitReadViews(2) == 2);

    std::array<uint8_t, 8> out{};
    const int32_t nread = stream.read(out.data(), out.size());
    REQUIRE(nread == 3);
    REQUIRE(out[0] == 7);
    REQUIRE(out[1] == 6);
    REQUIRE(out[2] == 5);
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: out-of-order fin is delivered only when contiguous", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 33);

    FrameStream fin;
    fin.stream_id = 33;
    fin.stream_offset = 3;
    fin.stream_data_length = 0;
    fin.stream_data = nullptr;
    fin.stream_flag = 0x01;
    REQUIRE(stream.onFrame(fin) == UTP_ERR_OK);

    uint8_t one = 0;
    REQUIRE(stream.read(&one, 1) == -1);
    REQUIRE(utp_get_last_error() == UTP_ERR_WOULD_BLOCK);

    std::array<uint8_t, 3> data = {1, 2, 3};
    FrameStream frame;
    frame.stream_id = 33;
    frame.stream_offset = 0;
    frame.stream_data_length = static_cast<uint16_t>(data.size());
    frame.stream_data = data.data();
    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);

    std::array<uint8_t, 8> out{};
    REQUIRE(stream.read(out.data(), out.size()) == 3);
    REQUIRE(out[0] == 1);
    REQUIRE(out[1] == 2);
    REQUIRE(out[2] == 3);
    REQUIRE(stream.read(&one, 1) == 0);
}

TEST_CASE("StreamImpl: overlapping fragments are trimmed without duplication", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 34);

    std::array<uint8_t, 8> base = {1, 2, 3, 4, 5, 6, 7, 8};
    FrameStream first;
    first.stream_id = 34;
    first.stream_offset = 0;
    first.stream_data_length = static_cast<uint16_t>(base.size());
    first.stream_data = base.data();
    REQUIRE(stream.onFrame(first) == UTP_ERR_OK);

    std::array<uint8_t, 8> overlap = {5, 6, 7, 8, 9, 10, 11, 12};
    FrameStream second;
    second.stream_id = 34;
    second.stream_offset = 4;
    second.stream_data_length = static_cast<uint16_t>(overlap.size());
    second.stream_data = overlap.data();
    REQUIRE(stream.onFrame(second) == UTP_ERR_OK);

    Stream::ConstBufferView views[2];
    const size_t total = stream.acquireReadViews(views, 32);
    REQUIRE(total == 12);

    std::array<uint8_t, 16> out{};
    REQUIRE(stream.read(out.data(), out.size()) == 12);
    for (size_t i = 0; i < 12; ++i) {
        REQUIRE(out[i] == static_cast<uint8_t>(i + 1));
    }
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: overlap chain keeps strictly contiguous output", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 35);

    std::array<uint8_t, 8> p0 = {1, 2, 3, 4, 5, 6, 7, 8};     // [0,8)
    std::array<uint8_t, 8> p1 = {5, 6, 7, 8, 9, 10, 11, 12};   // [4,12)
    std::array<uint8_t, 4> p2 = {3, 4, 5, 6};                  // [2,6)
    std::array<uint8_t, 6> p3 = {11, 12, 13, 14, 15, 16};      // [10,16)

    FrameStream f0;
    f0.stream_id = 35;
    f0.stream_offset = 0;
    f0.stream_data_length = static_cast<uint16_t>(p0.size());
    f0.stream_data = p0.data();
    REQUIRE(stream.onFrame(f0) == UTP_ERR_OK);

    FrameStream f1;
    f1.stream_id = 35;
    f1.stream_offset = 4;
    f1.stream_data_length = static_cast<uint16_t>(p1.size());
    f1.stream_data = p1.data();
    REQUIRE(stream.onFrame(f1) == UTP_ERR_OK);

    FrameStream f2;
    f2.stream_id = 35;
    f2.stream_offset = 2;
    f2.stream_data_length = static_cast<uint16_t>(p2.size());
    f2.stream_data = p2.data();
    REQUIRE(stream.onFrame(f2) == UTP_ERR_OK);

    FrameStream f3;
    f3.stream_id = 35;
    f3.stream_offset = 10;
    f3.stream_data_length = static_cast<uint16_t>(p3.size());
    f3.stream_data = p3.data();
    REQUIRE(stream.onFrame(f3) == UTP_ERR_OK);

    Stream::ConstBufferView views[2];
    // acquireReadViews returns at most two contiguous spans.
    REQUIRE(stream.acquireReadViews(views, 64) == 12);
    REQUIRE(stream.commitReadViews(5) == 5);

    std::array<uint8_t, 16> out{};
    const int32_t nread = stream.read(out.data(), out.size());
    REQUIRE(nread == 11);
    for (size_t i = 0; i < 11; ++i) {
        REQUIRE(out[i] == static_cast<uint8_t>(i + 6));
    }
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: multiple fragments can reference one PacketIn", "[Stream][ReadViews]")
{
    StreamImpl stream(nullptr, 36);

    eular::utp::PacketIn packet;

    std::array<uint8_t, 3> p0 = {1, 2, 3};
    FrameStream f0;
    f0.stream_id = 36;
    f0.stream_offset = 0;
    f0.stream_data_length = static_cast<uint16_t>(p0.size());
    f0.stream_data = p0.data();
    REQUIRE(stream.onFrame(f0, &packet) == UTP_ERR_OK);

    std::array<uint8_t, 3> p1 = {4, 5, 6};
    FrameStream f1;
    f1.stream_id = 36;
    f1.stream_offset = 3;
    f1.stream_data_length = static_cast<uint16_t>(p1.size());
    f1.stream_data = p1.data();
    REQUIRE(stream.onFrame(f1, &packet) == UTP_ERR_OK);

    auto first = stream.firstFragment();
    REQUIRE(first != nullptr);
    REQUIRE(first->packet == &packet);
    auto second = stream.nextFragment(first);
    REQUIRE(second != nullptr);
    REQUIRE(second->packet == &packet);

    Stream::ConstBufferView views[2];
    REQUIRE(stream.acquireReadViews(views, 16) == 6);
    REQUIRE(views[0].len == 3);
    REQUIRE(views[1].len == 3);

    REQUIRE(stream.commitReadViews(4) == 4);

    std::array<uint8_t, 8> out{};
    const int32_t nread = stream.read(out.data(), out.size());
    REQUIRE(nread == 2);
    REQUIRE(out[0] == 5);
    REQUIRE(out[1] == 6);
    REQUIRE_FALSE(stream.readable());
}
