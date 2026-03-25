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
using eular::utp::Connection;
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

TEST_CASE("ConnectionImpl: setOnIncomingStream only fires for peer-created streams", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1008);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_peerTP.init_max_streams_bidi = 8;
    conn.m_loaclTP.init_max_streams_bidi = 8;

    uint32_t callbackStreamId = UINT32_MAX;
    int callbackCount = 0;
    conn.setOnIncomingStream([&](Stream *stream) {
        REQUIRE(stream != nullptr);
        callbackStreamId = stream->id();
        ++callbackCount;
    });

    const int32_t localStreamId = conn.createStream();
    REQUIRE(localStreamId == 0);
    REQUIRE(callbackCount == 0);

    FrameStream incoming;
    uint8_t byte = 7;
    incoming.stream_id = 1;
    incoming.stream_offset = 0;
    incoming.stream_data_length = 1;
    incoming.stream_data = &byte;
    REQUIRE(conn.ingestStreamFrame(incoming) == UTP_ERR_OK);
    REQUIRE(callbackCount == 1);
    REQUIRE(callbackStreamId == 1);
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

TEST_CASE("ConnectionImpl: ingress stream gate checks role and per-type limits", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1003);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_loaclTP.init_max_streams_bidi = 1;
    conn.m_loaclTP.init_max_streams_uni = 1;

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

    FrameStream validUni = valid;
    validUni.stream_id = 3; // server-initiated uni stream
    REQUIRE(conn.ingestStreamFrame(validUni) == UTP_ERR_OK);

    FrameStream overLimit = valid;
    overLimit.stream_id = 5; // second server-initiated bidi stream (ordinal=2)
    REQUIRE(conn.ingestStreamFrame(overLimit) == UTP_ERR_STREAM_LIMIT_ERROR);

    FrameStream overLimitUni = valid;
    overLimitUni.stream_id = 7; // second server-initiated uni stream (ordinal=2)
    REQUIRE(conn.ingestStreamFrame(overLimitUni) == UTP_ERR_STREAM_LIMIT_ERROR);
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
    conn.m_peerTP.init_max_streams_uni = 1;

    const int32_t bidiStreamId = conn.createStream(Connection::kStreamTypeBidirectional);
    REQUIRE(bidiStreamId == 1);

    const int32_t uniStreamId = conn.createStream(Connection::kStreamTypeUnidirectional);
    REQUIRE(uniStreamId == 3);

    REQUIRE(conn.createStream(Connection::kStreamTypeUnidirectional) == UTP_ERR_STREAM_LIMIT_ERROR);
}

TEST_CASE("ConnectionImpl: streamCount and creatableStreamCount support stream type", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1007);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_peerTP.init_max_streams_bidi = 2;
    conn.m_peerTP.init_max_streams_uni = 1;

    // Simulate one peer-initiated bidi stream; it must not consume local creation quota.
    conn.m_streams.emplace(1u, std::make_shared<StreamImpl>(&conn, 1u));

    REQUIRE(conn.streamCount(Connection::kStreamTypeAll) == 1);
    REQUIRE(conn.streamCount(Connection::kStreamTypeBidirectional) == 1);
    REQUIRE(conn.streamCount(Connection::kStreamTypeUnidirectional) == 0);
    REQUIRE(conn.creatableStreamCount(Connection::kStreamTypeBidirectional) == 2);
    REQUIRE(conn.creatableStreamCount(Connection::kStreamTypeUnidirectional) == 1);

    REQUIRE(conn.createStream(Connection::kStreamTypeBidirectional) == 0);
    REQUIRE(conn.createStream(Connection::kStreamTypeUnidirectional) == 2);

    REQUIRE(conn.streamCount(Connection::kStreamTypeAll) == 3);
    REQUIRE(conn.streamCount(Connection::kStreamTypeBidirectional) == 2);
    REQUIRE(conn.streamCount(Connection::kStreamTypeUnidirectional) == 1);
    REQUIRE(conn.creatableStreamCount(Connection::kStreamTypeBidirectional) == 1);
    REQUIRE(conn.creatableStreamCount(Connection::kStreamTypeUnidirectional) == 0);
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

TEST_CASE("ConnectionImpl: strict scheduler picks higher priority stream first", "[Connection][Stream][Priority]")
{
    Config cfg;
    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerStrict;
    cfg.stream_aging_threshold = 1024;
    cfg.stream_aging_step = 1;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1010);
    conn.m_state = ConnectionImpl::kStateConnected;

    StreamImpl::SP sHigh = std::make_shared<StreamImpl>(&conn, 4, 0);
    StreamImpl::SP sMid = std::make_shared<StreamImpl>(&conn, 8, 4);
    StreamImpl::SP sLow = std::make_shared<StreamImpl>(&conn, 12, 7);

    StreamImpl::PendingSendChunk c;
    c.offset = 0;
    c.bytes = 32;
    c.fin = false;
    sHigh->m_sendQueue.push_back(c);
    sMid->m_sendQueue.push_back(c);
    sLow->m_sendQueue.push_back(c);

    conn.m_streams.emplace(sHigh->id(), sHigh);
    conn.m_streams.emplace(sMid->id(), sMid);
    conn.m_streams.emplace(sLow->id(), sLow);

    StreamImpl::SP selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sHigh->id());
    selected->m_sendQueue.clear();
    conn.updateStrictAgingState(selected->id());

    selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sMid->id());
    selected->m_sendQueue.clear();
    conn.updateStrictAgingState(selected->id());

    selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sLow->id());
}

TEST_CASE("ConnectionImpl: strict scheduler aging promotes long-wait stream", "[Connection][Stream][Priority]")
{
    Config cfg;
    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerStrict;
    cfg.stream_aging_threshold = 1;
    cfg.stream_aging_step = 4;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1011);
    conn.m_state = ConnectionImpl::kStateConnected;

    StreamImpl::SP sBase = std::make_shared<StreamImpl>(&conn, 4, 3);
    StreamImpl::SP sAged = std::make_shared<StreamImpl>(&conn, 8, 7);

    StreamImpl::PendingSendChunk c;
    c.offset = 0;
    c.bytes = 32;
    c.fin = false;
    sBase->m_sendQueue.push_back(c);
    sAged->m_sendQueue.push_back(c);

    conn.m_streams.emplace(sBase->id(), sBase);
    conn.m_streams.emplace(sAged->id(), sAged);

    StreamImpl::SP first = conn.pickNextWritableStream();
    REQUIRE(first != nullptr);
    REQUIRE(first->id() == sBase->id());
    conn.updateStrictAgingState(first->id());

    StreamImpl::SP second = conn.pickNextWritableStream();
    REQUIRE(second != nullptr);
    REQUIRE(second->id() == sAged->id());
}

TEST_CASE("ConnectionImpl: drr scheduler rotates equal-priority streams", "[Connection][Stream][Priority]")
{
    Config cfg;
    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerDrr;
    cfg.stream_drr_quantum = 64;
    cfg.stream_drr_deficit_cap = 1024;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1012);
    conn.m_state = ConnectionImpl::kStateConnected;

    StreamImpl::SP s1 = std::make_shared<StreamImpl>(&conn, 4, 4);
    StreamImpl::SP s2 = std::make_shared<StreamImpl>(&conn, 8, 4);

    StreamImpl::PendingSendChunk c;
    c.offset = 0;
    c.bytes = 64;
    c.fin = false;
    s1->m_sendQueue.push_back(c);
    s2->m_sendQueue.push_back(c);

    conn.m_streams.emplace(s1->id(), s1);
    conn.m_streams.emplace(s2->id(), s2);

    StreamImpl::SP first = conn.pickNextWritableStream();
    REQUIRE(first != nullptr);
    REQUIRE(first->id() == s1->id());
    first->m_sendQueue.clear();

    StreamImpl::SP second = conn.pickNextWritableStream();
    REQUIRE(second != nullptr);
    REQUIRE(second->id() == s2->id());
}

TEST_CASE("ConnectionImpl: scheduler mode switch changes stream selection", "[Connection][Stream][Priority]")
{
    Config cfg;
    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerDisabled;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1013);
    conn.m_state = ConnectionImpl::kStateConnected;

    StreamImpl::SP lowIdLowPrio = std::make_shared<StreamImpl>(&conn, 4, 7);
    StreamImpl::SP highIdHighPrio = std::make_shared<StreamImpl>(&conn, 8, 0);

    StreamImpl::PendingSendChunk c;
    c.offset = 0;
    c.bytes = 16;
    c.fin = false;
    lowIdLowPrio->m_sendQueue.push_back(c);
    highIdHighPrio->m_sendQueue.push_back(c);

    conn.m_streams.emplace(lowIdLowPrio->id(), lowIdLowPrio);
    conn.m_streams.emplace(highIdHighPrio->id(), highIdHighPrio);

    StreamImpl::SP selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == lowIdLowPrio->id());

    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerStrict;
    selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == highIdHighPrio->id());
}

TEST_CASE("StreamImpl: setPriority validates input range", "[Connection][Stream][Priority]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1014);
    StreamImpl stream(&conn, 4, 4);

    REQUIRE(stream.setPriority(0) == UTP_ERR_OK);
    REQUIRE(stream.priority() == 0);
    REQUIRE(stream.setPriority(7) == UTP_ERR_OK);
    REQUIRE(stream.priority() == 7);
    REQUIRE(stream.setPriority(8) == -UTP_ERR_INVALID_PARAM);
}

TEST_CASE("ConnectionImpl: statistic exports scheduler metrics", "[Connection][Stream][Priority]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1015);
    conn.m_schedulerStats.selectTotal = 11;
    conn.m_schedulerStats.selectDisabled = 2;
    conn.m_schedulerStats.selectStrict = 5;
    conn.m_schedulerStats.selectDrr = 4;
    conn.m_schedulerStats.strictAgingPromoted = 3;
    conn.m_schedulerStats.wouldBlock = 7;
    conn.m_schedulerStats.emptyRounds = 9;
    conn.m_schedulerStats.modeSwitches = 1;
    conn.m_schedulerStats.drrDeficitRefills = 13;
    conn.m_schedulerStats.drrDeficitConsumes = 8;

    const Connection::Statistic stat = conn.statistic();
    REQUIRE(stat.scheduler_select_total == 11);
    REQUIRE(stat.scheduler_select_disabled == 2);
    REQUIRE(stat.scheduler_select_strict == 5);
    REQUIRE(stat.scheduler_select_drr == 4);
    REQUIRE(stat.scheduler_strict_aging_promoted == 3);
    REQUIRE(stat.scheduler_would_block == 7);
    REQUIRE(stat.scheduler_empty_rounds == 9);
    REQUIRE(stat.scheduler_mode_switches == 1);
    REQUIRE(stat.scheduler_drr_refills == 13);
    REQUIRE(stat.scheduler_drr_consumes == 8);
}
