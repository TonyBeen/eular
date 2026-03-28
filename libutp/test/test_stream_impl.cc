/*************************************************************************
    > File Name: test_stream_impl.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 19 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>
#include <cstring>

#include "utp/errno.h"

#define private public
#include "context/stream_impl.h"
#undef private

using eular::utp::FrameStream;
using eular::utp::StreamImpl;

TEST_CASE("StreamImpl: onFrame pushes data and read consumes", "[Stream]")
{
    StreamImpl stream(nullptr, 7);

    std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    FrameStream frame;
    frame.stream_id = 7;
    frame.stream_offset = 0;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();

    bool readableNotified = false;
    stream.setOnReadable([&readableNotified]() {
        readableNotified = true;
    });

    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);
    REQUIRE(stream.readable());
    REQUIRE(readableNotified);

    std::array<uint8_t, 8> out{};
    int32_t nread = stream.read(out.data(), out.size());
    REQUIRE(nread == static_cast<int32_t>(payload.size()));
    REQUIRE(std::equal(payload.begin(), payload.end(), out.begin()));
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: out-of-order frame is rejected for now", "[Stream]")
{
    StreamImpl stream(nullptr, 9);

    std::array<uint8_t, 2> payload = {9, 8};
    FrameStream frame;
    frame.stream_id = 9;
    frame.stream_offset = 10;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();

    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);
    REQUIRE_FALSE(stream.readable());

    FrameStream head;
    head.stream_id = 9;
    head.stream_offset = 0;
    head.stream_data_length = 2;
    head.stream_data = payload.data();

    REQUIRE(stream.onFrame(head) == UTP_ERR_OK);
    REQUIRE(stream.readable());

    std::array<uint8_t, 8> out{};
    REQUIRE(stream.read(out.data(), out.size()) == 2);
    REQUIRE(out[0] == 9);
    REQUIRE(out[1] == 8);
}

TEST_CASE("StreamImpl: peer fin returns EOF on empty read", "[Stream]")
{
    StreamImpl stream(nullptr, 11);

    FrameStream frame;
    frame.stream_id = 11;
    frame.stream_offset = 0;
    frame.stream_data_length = 0;
    frame.stream_data = nullptr;
    frame.stream_flag = 0x01; // FIN

    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);
    REQUIRE(stream.state() == eular::utp::Stream::kStateHalfClosedRemote);

    uint8_t buf = 0;
    REQUIRE(stream.read(&buf, 1) == 0);
}

TEST_CASE("StreamImpl: zero-copy read interface exposes readable buffer", "[Stream]")
{
    StreamImpl stream(nullptr, 12);

    std::array<uint8_t, 3> payload = {4, 5, 6};
    FrameStream frame;
    frame.stream_id = 12;
    frame.stream_offset = 0;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();

    REQUIRE(stream.onFrame(frame) == UTP_ERR_OK);

    eular::utp::Stream::ConstBufferView views[2];
    const size_t bytes = stream.acquireReadBuffer(views, 16);
    REQUIRE(bytes == payload.size());
    REQUIRE(views[0].data != nullptr);
    REQUIRE(views[0].len == payload.size());
    REQUIRE(std::memcmp(views[0].data, payload.data(), payload.size()) == 0);

    REQUIRE(stream.consumeRead(bytes) == static_cast<int32_t>(bytes));
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: zero-copy write interface can reserve buffer", "[Stream]")
{
    StreamImpl stream(nullptr, 15);

    eular::utp::Stream::MutableBufferView views[2];
    const size_t bytes = stream.acquireWriteBuffer(views, 32);
    REQUIRE(bytes >= 32);
    REQUIRE(views[0].data != nullptr);

    static const uint8_t payload[5] = {7, 8, 9, 10, 11};
    std::memcpy(views[0].data, payload, sizeof(payload));
    REQUIRE(stream.commitWrite(sizeof(payload), false) == -UTP_ERR_INVALID_STATE);
    REQUIRE(stream.state() == eular::utp::Stream::kStateOpen);
}

TEST_CASE("StreamImpl: setOnWritable fires immediately when writable", "[Stream]")
{
    StreamImpl stream(nullptr, 16);

    int writableNotified = 0;
    stream.setOnWritable([&]() {
        ++writableNotified;
    });

    REQUIRE(stream.writable());
    REQUIRE(writableNotified == 1);
}

TEST_CASE("StreamImpl: writable reflects send queue capacity", "[Stream]")
{
    StreamImpl stream(nullptr, 17);

    REQUIRE(stream.writable());
    stream.m_sendQueuedBytes = StreamImpl::kDefaultBufferCapacity;
    REQUIRE_FALSE(stream.writable());
}

TEST_CASE("StreamImpl: close transitions local side state", "[Stream]")
{
    StreamImpl stream(nullptr, 13);
    REQUIRE(stream.state() == eular::utp::Stream::kStateOpen);
    REQUIRE(stream.writable());

    REQUIRE(stream.write("ab", 2, true) == -UTP_ERR_INVALID_STATE);

    StreamImpl rxOnly(nullptr, 14);
    FrameStream fin;
    fin.stream_id = 14;
    fin.stream_offset = 0;
    fin.stream_data_length = 0;
    fin.stream_data = nullptr;
    fin.stream_flag = 0x01;
    REQUIRE(rxOnly.onFrame(fin) == UTP_ERR_OK);
    REQUIRE(rxOnly.state() == eular::utp::Stream::kStateHalfClosedRemote);
}

TEST_CASE("StreamImpl: reset frame notifies upper layer", "[Stream]")
{
    StreamImpl stream(nullptr, 21);

    bool resetNotified = false;
    uint16_t resetCode = 0;
    stream.setOnReset([&](uint16_t err) {
        resetNotified = true;
        resetCode = err;
    });

    REQUIRE(stream.onReset(UTP_ERR_CANCELLED, true) == UTP_ERR_OK);
    REQUIRE(stream.resetReceived());
    REQUIRE(resetNotified);
    REQUIRE(resetCode == UTP_ERR_CANCELLED);
    REQUIRE(stream.state() == eular::utp::Stream::kStateClosed);
}

TEST_CASE("StreamImpl: queued local fin is not fully closed before send", "[Stream]")
{
    StreamImpl stream(nullptr, 22);

    stream.m_peerFin = true;
    stream.m_localFinQueued = true;
    stream.m_localFinSent = false;

    REQUIRE(stream.state() == eular::utp::Stream::kStateHalfClosedLocal);

    stream.m_localFinSent = true;
    REQUIRE(stream.state() == eular::utp::Stream::kStateClosed);
}
