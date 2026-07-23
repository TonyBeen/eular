/*************************************************************************
    > File Name: test_stream_impl.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 19 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>
#include <cstring>

#include "utp/errno.h"

#include <event/loop.h>
#define private public
#include "context/context_impl.h"
#include "context/connection_impl.h"
#include "context/stream_impl.h"
#undef private

using eular::utp::FrameStream;
using eular::utp::Status;
using eular::utp::StreamImpl;
using eular::utp::Status;

struct StreamTestCtx {
    eular::utp::Config      cfg;
    ev::EventLoop           loop;
    eular::utp::ContextImpl ctx;
    eular::utp::ConnectionImpl conn;
    StreamTestCtx() : ctx(loop.loop(), &cfg), conn(&ctx, nullptr, 9999) {}
};

TEST_CASE("StreamImpl: onFrame pushes data and read consumes", "[Stream]")
{
    StreamTestCtx _fix_7; StreamImpl stream(&_fix_7.conn, 7);

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

    REQUIRE(stream.onFrame(frame)  == 0);
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
    StreamTestCtx _fix_9; StreamImpl stream(&_fix_9.conn, 9);

    std::array<uint8_t, 2> payload = {9, 8};
    FrameStream frame;
    frame.stream_id = 9;
    frame.stream_offset = 10;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();

    REQUIRE(stream.onFrame(frame)  == 0);
    REQUIRE_FALSE(stream.readable());

    FrameStream head;
    head.stream_id = 9;
    head.stream_offset = 0;
    head.stream_data_length = 2;
    head.stream_data = payload.data();

    REQUIRE(stream.onFrame(head)  == 0);
    REQUIRE(stream.readable());

    std::array<uint8_t, 8> out{};
    REQUIRE(stream.read(out.data(), out.size()) == 2);
    REQUIRE(out[0] == 9);
    REQUIRE(out[1] == 8);
}

TEST_CASE("StreamImpl: reassembly limit accounts pinned packet capacity", "[Stream][Memory]")
{
    eular::utp::Config cfg;
    cfg.recv_stream_max_gap = 128;
    cfg.recv_stream_reassembly_memory_limit = 1500;
    cfg.recv_reassembly_memory_limit = 1500;
    ev::EventLoop loop;
    eular::utp::ContextImpl ctx(loop.loop(), &cfg);
    eular::utp::ConnectionImpl conn(&ctx, nullptr, 9998);

    {
        StreamImpl stream(&conn, 9);
        eular::utp::PacketIn *firstPacket = conn.m_mm.getPacketIn(100);
        REQUIRE(firstPacket != nullptr);
        uint8_t firstByte = 1;
        FrameStream first;
        first.stream_id = 9;
        first.stream_offset = 10;
        first.stream_data_length = 1;
        first.stream_data = &firstByte;
        REQUIRE(stream.onFrame(first, firstPacket).ok());
        conn.m_mm.releasePacketIn(firstPacket);
        REQUIRE(stream.m_recvFragmentCount == 1);
        REQUIRE(stream.m_recvPinnedMemoryBytes >= firstPacket->alloc_size);

        eular::utp::PacketIn *secondPacket = conn.m_mm.getPacketIn(100);
        REQUIRE(secondPacket != nullptr);
        uint8_t secondByte = 2;
        FrameStream second = first;
        second.stream_offset = 20;
        second.stream_data = &secondByte;
        REQUIRE(stream.onFrame(second, secondPacket).code() == UTP_ERR_WOULD_BLOCK);
        conn.m_mm.releasePacketIn(secondPacket);
        REQUIRE(stream.m_recvFragmentCount == 1);
        REQUIRE(conn.m_recvReassemblyFragmentCount == 1);
    }

    REQUIRE(conn.m_recvReassemblyMemoryBytes == 0);
    REQUIRE(conn.m_recvReassemblyFragmentCount == 0);
}

TEST_CASE("StreamImpl: rejects null writes and overflowing receive offsets", "[Stream][Boundary]")
{
    StreamTestCtx fixture;
    StreamImpl stream(&fixture.conn, 9);
    REQUIRE(stream.write(nullptr, 1, false) == -1);
    REQUIRE(utp_get_last_error() == UTP_ERR_INVALID_PARAM);

    uint8_t byte = 1;
    FrameStream frame;
    frame.stream_id = 9;
    frame.stream_offset = UINT64_MAX;
    frame.stream_data_length = 1;
    frame.stream_data = &byte;
    REQUIRE(stream.onFrame(frame).code() == UTP_ERR_STREAM_FLOW_CONTROL);
}

TEST_CASE("StreamImpl: peer fin returns EOF on empty read", "[Stream]")
{
    StreamTestCtx _fix_11; StreamImpl stream(&_fix_11.conn, 11);

    FrameStream frame;
    frame.stream_id = 11;
    frame.stream_offset = 0;
    frame.stream_data_length = 0;
    frame.stream_data = nullptr;
    frame.stream_flag = 0x01; // FIN

    REQUIRE(stream.onFrame(frame)  == 0);
    REQUIRE(stream.state() == eular::utp::Stream::kStateHalfClosedRemote);

    uint8_t buf = 0;
    REQUIRE(stream.read(&buf, 1) == 0);
}

TEST_CASE("StreamImpl: read views interface exposes readable buffer", "[Stream]")
{
    StreamTestCtx _fix_12; StreamImpl stream(&_fix_12.conn, 12);

    std::array<uint8_t, 3> payload = {4, 5, 6};
    FrameStream frame;
    frame.stream_id = 12;
    frame.stream_offset = 0;
    frame.stream_data_length = static_cast<uint16_t>(payload.size());
    frame.stream_data = payload.data();

    REQUIRE(stream.onFrame(frame)  == 0);

    eular::utp::Stream::ConstBufferView views[2];
    const size_t bytes = stream.acquireReadViews(views, 16);
    REQUIRE(bytes == payload.size());
    REQUIRE(views[0].data != nullptr);
    REQUIRE(views[0].len == payload.size());
    REQUIRE(std::memcmp(views[0].data, payload.data(), payload.size()) == 0);

    REQUIRE(stream.commitReadViews(bytes) == static_cast<int32_t>(bytes));
    REQUIRE_FALSE(stream.readable());
}

TEST_CASE("StreamImpl: zero-copy write interface can reserve buffer", "[Stream]")
{
    StreamTestCtx _fix_15; StreamImpl stream(&_fix_15.conn, 15);

    eular::utp::Stream::MutableBufferView views[2];
    const size_t bytes = stream.acquireWriteBuffer(views, 32);
    REQUIRE(bytes >= 32);
    REQUIRE(views[0].data != nullptr);

    static const uint8_t payload[5] = {7, 8, 9, 10, 11};
    std::memcpy(views[0].data, payload, sizeof(payload));
    REQUIRE(stream.commitWrite(sizeof(payload), false) == static_cast<int32_t>(sizeof(payload)));
    REQUIRE(stream.state() == eular::utp::Stream::kStateOpen);
}

TEST_CASE("StreamImpl: setOnWritable fires immediately when writable", "[Stream]")
{
    StreamTestCtx _fix_16; StreamImpl stream(&_fix_16.conn, 16);

    int writableNotified = 0;
    stream.setOnWritable([&]() {
        ++writableNotified;
    });

    REQUIRE(stream.writable());
    REQUIRE(writableNotified == 1);
}
TEST_CASE("StreamImpl: writable reflects send queue capacity", "[Stream]")
{
    eular::utp::Status st;
    StreamTestCtx _fix_17; StreamImpl stream(&_fix_17.conn, 17);

    REQUIRE(stream.writable());
    stream.m_sendQueuedBytes = 256 * 1024; // Default config limit
    REQUIRE_FALSE(stream.writable());
}

TEST_CASE("StreamImpl: close transitions local side state", "[Stream]")
{
    StreamTestCtx _fix_13; StreamImpl stream(&_fix_13.conn, 13);
    REQUIRE(stream.state() == eular::utp::Stream::kStateOpen);
    REQUIRE(stream.writable());

    // Writing with FIN queues the local FIN; operation succeeds
    REQUIRE(stream.write("ab", 2, true) == 2);
    REQUIRE_FALSE(stream.writable());
    // Second write after FIN is queued must be rejected
    REQUIRE(stream.write("c", 1, false) == -1);
    REQUIRE(utp_get_last_error() == UTP_ERR_STREAM_CLOSED);

    StreamTestCtx _fix_14; StreamImpl rxOnly(&_fix_14.conn, 14);
    FrameStream fin;
    fin.stream_id = 14;
    fin.stream_offset = 0;
    fin.stream_data_length = 0;
    fin.stream_data = nullptr;
    fin.stream_flag = 0x01;
    REQUIRE(rxOnly.onFrame(fin)  == 0);
    REQUIRE(rxOnly.state() == eular::utp::Stream::kStateHalfClosedRemote);
}

TEST_CASE("StreamImpl: reset frame notifies upper layer", "[Stream]")
{
    StreamTestCtx _fix_21; StreamImpl stream(&_fix_21.conn, 21);

    bool resetNotified = false;
    uint16_t resetCode = 0;
    stream.setOnReset([&](uint16_t err) {
        resetNotified = true;
        resetCode = err;
    });

    REQUIRE(stream.onReset(UTP_ERR_CANCELLED, true)  == 0);
    REQUIRE(stream.resetReceived());
    REQUIRE(resetNotified);
    REQUIRE(resetCode == UTP_ERR_CANCELLED);
    REQUIRE(stream.state() == eular::utp::Stream::kStateClosed);
}

TEST_CASE("StreamImpl: queued local fin is not fully closed before send", "[Stream]")
{
    StreamTestCtx _fix_22; StreamImpl stream(&_fix_22.conn, 22);

    stream.m_peerFin = true;
    stream.m_localFinQueued = true;
    stream.m_localFinSent = false;

    REQUIRE(stream.state() == eular::utp::Stream::kStateHalfClosedLocal);

    stream.m_localFinSent = true;
    REQUIRE(stream.state() == eular::utp::Stream::kStateClosed);
}
