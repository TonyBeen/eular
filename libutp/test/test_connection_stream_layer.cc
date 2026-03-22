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
