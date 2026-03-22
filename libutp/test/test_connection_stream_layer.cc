/*************************************************************************
    > File Name: test_connection_stream_layer.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <event/loop.h>

#define private public
#include "context/context_impl.h"
#include "context/stream_impl.h"
#undef private

#include "utp/errno.h"

using eular::utp::Config;
using eular::utp::ConnectionImpl;
using eular::utp::ContextImpl;
using eular::utp::Stream;
using eular::utp::StreamImpl;

TEST_CASE("ConnectionImpl: getStream returns created stream pointer", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1001);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerTP.init_max_streams_bidi = 8;

    const int32_t streamId = conn.createStream();
    REQUIRE(streamId >= 0);

    Stream *stream = conn.getStream(static_cast<uint32_t>(streamId));
    REQUIRE(stream != nullptr);
    REQUIRE(stream->id() == static_cast<uint32_t>(streamId));

    REQUIRE(conn.getStream(999999) == nullptr);
}

TEST_CASE("ConnectionImpl: streamCount counts only active streams", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1002);

    StreamImpl::SP openStream = std::make_shared<StreamImpl>(&conn, 1);
    StreamImpl::SP closedStream = std::make_shared<StreamImpl>(&conn, 5);
    closedStream->m_localFinQueued = true;
    closedStream->m_localFinSent = true;
    closedStream->m_peerFin = true;

    conn.m_streams.emplace(1u, openStream);
    conn.m_streams.emplace(5u, closedStream);

    REQUIRE(openStream->state() == Stream::kStateOpen);
    REQUIRE(closedStream->state() == Stream::kStateClosed);
    REQUIRE(conn.streamCount() == 1);
}

TEST_CASE("ConnectionImpl: ingress stream gate checks role direction and limit", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1003);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_loaclTP.init_max_streams_bidi = 1;

    FrameStream valid;
    uint8_t byte = 1;
    valid.stream_id = 1; // server-initiated bidi stream
    valid.stream_offset = 0;
    valid.stream_data_length = 1;
    valid.stream_data = &byte;
    REQUIRE(conn.ingestStreamFrame(valid) == UTP_ERR_OK);

    FrameStream wrongRole = valid;
    wrongRole.stream_id = 0; // client-initiated, should not appear as ingress for client side
    REQUIRE(conn.ingestStreamFrame(wrongRole) == UTP_ERR_STREAM_STATE_ERROR);

    FrameStream wrongDir = valid;
    wrongDir.stream_id = 3; // server-initiated uni stream
    REQUIRE(conn.ingestStreamFrame(wrongDir) == UTP_ERR_STREAM_STATE_ERROR);

    FrameStream overLimit = valid;
    overLimit.stream_id = 5; // second server-initiated bidi stream (ordinal=2)
    REQUIRE(conn.ingestStreamFrame(overLimit) == UTP_ERR_STREAM_LIMIT_ERROR);
}

TEST_CASE("ConnectionImpl: collectClosedStreams erases fully drained closed stream", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1004);

    StreamImpl::SP closedStream = std::make_shared<StreamImpl>(&conn, 9);
    closedStream->m_localFinQueued = true;
    closedStream->m_localFinSent = true;
    closedStream->m_peerFin = true;

    conn.m_streams.emplace(9u, closedStream);
    REQUIRE(conn.m_streams.size() == 1);

    conn.collectClosedStreams();
    REQUIRE(conn.m_streams.empty());
}

TEST_CASE("ConnectionImpl: passive side creates server-initiated stream id", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1005);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = false;
    conn.m_peerTP.init_max_streams_bidi = 8;

    const int32_t streamId = conn.createStream();
    REQUIRE(streamId == 1);
}

TEST_CASE("ConnectionImpl: multi-stream ingress and reclamation regression", "[Connection][Stream][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1006);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_loaclTP.init_max_streams_bidi = 64;

    uint8_t byte = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        FrameStream frame;
        frame.stream_id = 1 + i * 4;
        frame.stream_offset = 0;
        frame.stream_data_length = 1;
        frame.stream_data = &byte;
        REQUIRE(conn.ingestStreamFrame(frame) == UTP_ERR_OK);
    }
    REQUIRE(conn.streamCount() == 32);

    uint32_t closed = 0;
    for (auto &entry : conn.m_streams) {
        if (!entry.second || (entry.first % 8) != 1) {
            continue;
        }

        entry.second->m_localFinQueued = true;
        entry.second->m_localFinSent = true;
        entry.second->m_peerFin = true;
        ++closed;
    }
    REQUIRE(closed > 0);

    conn.collectClosedStreams();
    REQUIRE(conn.streamCount() == 32 - static_cast<int32_t>(closed));
}
