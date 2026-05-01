/*************************************************************************
    > File Name: test_connection_stream_layer.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>
#include <chrono>
#include <thread>

#include <event/loop.h>
#include <event2/event.h>

#define private public
#include "context/context_impl.h"
#include "context/stream_impl.h"
#undef private

#include "utp/errno.h"
#include "proto/packet_out.h"
#include "util/time.h"

using eular::utp::Config;
using eular::utp::Connection;
using eular::utp::ConnectionImpl;
using eular::utp::ContextImpl;
using eular::utp::FrameStream;
using eular::utp::Stream;
using eular::utp::StreamImpl;
using eular::utp::Address;
using eular::utp::UdpSocket;

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

TEST_CASE("StreamImpl: configurable send buffer limit is enforced", "[Connection][Stream]")
{
    Config cfg;
    cfg.stream_send_buffer_limit = 8 * 1024;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1020);
    StreamImpl stream(&conn, 4);

    REQUIRE(stream.appWriteCredit() == static_cast<size_t>(cfg.stream_send_buffer_limit));

    stream.m_sendQueuedBytes = cfg.stream_send_buffer_limit;
    REQUIRE(stream.appWriteCredit() == 0);
    REQUIRE_FALSE(stream.writable());
}

TEST_CASE("ConnectionImpl: handshake done pending clears only on ack callback", "[Connection][Handshake]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1099);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    conn.m_handshakeDoneSent = true;

    conn.onHandshakeDoneFrameAcked();
    REQUIRE_FALSE(conn.m_handshakeDonePending);
}

TEST_CASE("ConnectionImpl: handshake barrier blocks regular stream send until handshake done acked", "[Connection][Handshake]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1100);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    conn.m_handshakeDoneSent = true;

    const uint8_t data[] = {'h', 'i'};
    const int32_t status = conn.sendStreamFrame(0, 0, data, sizeof(data), false);
    REQUIRE(status == UTP_ERR_WOULD_BLOCK);
}

TEST_CASE("ConnectionImpl: handshake done delay uses peer transport timeout", "[Connection][Handshake]")
{
    Config cfg;
    cfg.handshake_timeout = 9000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1101);
    conn.m_peerTP.handshake_timeout = 6000;

    REQUIRE(conn.handshakeDoneDelayMs() == 2000);
}

TEST_CASE("ConnectionImpl: stream unacked data limit checks pending bytes", "[Connection][Stream]")
{
    Config cfg;
    cfg.stream_unacked_data_limit = 1024;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1021);
    conn.m_streamUnackedDataBytes = 900;
    REQUIRE(conn.canSendStreamUnackedBytes(100));
    REQUIRE_FALSE(conn.canSendStreamUnackedBytes(200));

    eular::utp::PacketOut pkt{};
    pkt.stream_data_size = 256;
    conn.onStreamPacketUnackedAdded(&pkt);
    REQUIRE(conn.m_streamUnackedDataBytes == 1156);
    conn.onStreamPacketUnackedRemoved(&pkt);
    REQUIRE(conn.m_streamUnackedDataBytes == 900);
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

    REQUIRE(conn.createStream(Connection::kStreamTypeUnidirectional) == -1);
    REQUIRE(utp_get_last_error() == UTP_ERR_STREAM_LIMIT_ERROR);
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

TEST_CASE("ConnectionImpl: schedule timer enqueues context write instead of direct onWrite", "[Connection][Scheduler]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1201);
    conn.m_state = ConnectionImpl::kStateConnected;

    REQUIRE(ctx.m_wantWriteConns.empty());
    REQUIRE(conn.m_schedulerStats.emptyRounds == 0);

    conn.nextScheduleTime(1);

    for (int i = 0; i < 10 && ctx.m_wantWriteConns.empty(); ++i) {
        loop.dispatch(EVLOOP_NONBLOCK | EVLOOP_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE_FALSE(ctx.m_wantWriteConns.empty());
    REQUIRE(ctx.m_wantWriteConns.front() == &conn);
    REQUIRE(conn.m_schedulerStats.emptyRounds == 0);
}

TEST_CASE("ContextImpl: onWriteEvent processes multiple connections in one fair round", "[Context][Scheduler]")
{
    Config cfg;
    cfg.connection_wdrr_quantum = 1200;
    cfg.connection_wdrr_deficit_cap = 64 * 1024;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    auto c1 = std::make_shared<ConnectionImpl>(&ctx, nullptr, 1301);
    auto c2 = std::make_shared<ConnectionImpl>(&ctx, nullptr, 1302);
    c1->m_state = ConnectionImpl::kStateConnected;
    c2->m_state = ConnectionImpl::kStateConnected;

    ctx.m_connections.emplace(c1->cid(), c1);
    ctx.m_connections.emplace(c2->cid(), c2);

    ctx.wantWrite(c1.get());
    ctx.wantWrite(c2.get());
    REQUIRE(ctx.m_wantWriteConns.size() == 2);

    ctx.onWriteEvent();

    REQUIRE(c1->m_schedulerStats.emptyRounds == 1);
    REQUIRE(c2->m_schedulerStats.emptyRounds == 1);
    REQUIRE(ctx.m_wantWriteConns.empty());
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

    sHigh->m_sendQueuedBytes = 32;
    sMid->m_sendQueuedBytes = 32;
    sLow->m_sendQueuedBytes = 32;

    conn.m_streams.emplace(sHigh->id(), sHigh);
    conn.m_streams.emplace(sMid->id(), sMid);
    conn.m_streams.emplace(sLow->id(), sLow);

    StreamImpl::SP selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sHigh->id());
    selected->m_sendQueuedBytes = 0;
    conn.updateStrictAgingState(selected->id());

    selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sMid->id());
    selected->m_sendQueuedBytes = 0;
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

    sBase->m_sendQueuedBytes = 32;
    sAged->m_sendQueuedBytes = 32;

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

    s1->m_sendQueuedBytes = 64;
    s2->m_sendQueuedBytes = 64;

    conn.m_streams.emplace(s1->id(), s1);
    conn.m_streams.emplace(s2->id(), s2);

    StreamImpl::SP first = conn.pickNextWritableStream();
    REQUIRE(first != nullptr);
    REQUIRE(first->id() == s1->id());
    first->m_sendQueuedBytes = 0;

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

    lowIdLowPrio->m_sendQueuedBytes = 16;
    highIdHighPrio->m_sendQueuedBytes = 16;

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
    REQUIRE(stream.setPriority(8) == -1);
    REQUIRE(utp_get_last_error() == UTP_ERR_INVALID_PARAM);
}

TEST_CASE("ConnectionImpl: statistic exports scheduler metrics", "[Connection][Stream][Priority]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1015);
    conn.m_obsRttUs = 12000;
    conn.m_obsRttVarUs = 3000;
    conn.m_bytesRetrans = 42;
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
    REQUIRE(stat.rtt == 12000);
    REQUIRE(stat.rttvar == 3000);
    REQUIRE(stat.rtx_bytes == 42);
}

TEST_CASE("StreamImpl: coalescing defers tiny payload within window", "[Connection][Stream][Coalescing]")
{
    Config cfg;
    cfg.stream_enable_coalescing = true;
    cfg.stream_min_payload_before_immediate_send = 512;
    cfg.stream_coalesce_delay_us = 5000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1016);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;
    StreamImpl stream(&conn, 4);

    const utp_time_t nowUs = eular::utp::time::MonotonicUs();
    stream.m_sendQueuedBytes = 128;
    stream.m_lastSendQueuedAtUs = nowUs - 1000;

    REQUIRE(stream.hasPendingSendWork());
    REQUIRE(stream.shouldDeferSend(nowUs));
    REQUIRE(stream.coalesceDelayRemainingUs(nowUs) > 0);
}

TEST_CASE("StreamImpl: coalescing bypasses threshold and expired window", "[Connection][Stream][Coalescing]")
{
    Config cfg;
    cfg.stream_enable_coalescing = true;
    cfg.stream_min_payload_before_immediate_send = 512;
    cfg.stream_coalesce_delay_us = 5000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1017);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;
    StreamImpl stream(&conn, 4);

    const utp_time_t nowUs = eular::utp::time::MonotonicUs();

    stream.m_sendQueuedBytes = cfg.stream_min_payload_before_immediate_send;
    stream.m_lastSendQueuedAtUs = nowUs - 500;
    REQUIRE_FALSE(stream.shouldDeferSend(nowUs));

    stream.m_sendQueuedBytes = 128;
    stream.m_lastSendQueuedAtUs = nowUs - 6000;
    REQUIRE_FALSE(stream.shouldDeferSend(nowUs));
    REQUIRE(stream.coalesceDelayRemainingUs(nowUs) == 0);
}

TEST_CASE("StreamImpl: FIN path is never deferred by coalescing", "[Connection][Stream][Coalescing]")
{
    Config cfg;
    cfg.stream_enable_coalescing = true;
    cfg.stream_min_payload_before_immediate_send = 512;
    cfg.stream_coalesce_delay_us = 5000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1018);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;
    StreamImpl stream(&conn, 4);

    const utp_time_t nowUs = eular::utp::time::MonotonicUs();
    stream.m_localFinQueued = true;
    stream.m_localFinSent = false;
    stream.m_sendQueuedBytes = 0;
    stream.m_lastSendQueuedAtUs = nowUs - 1000;

    REQUIRE(stream.hasPendingSendWork());
    REQUIRE_FALSE(stream.shouldDeferSend(nowUs));
}

TEST_CASE("StreamImpl: flushPendingSends does not send early FIN before queued payload", "[Connection][Stream][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    UdpSocket sock(cfg);

    REQUIRE(sock.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    ConnectionImpl conn(&ctx, &sock, 1022);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerAddress = Address("127.0.0.1", 9);
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;

    StreamImpl stream(&conn, 4);

    std::array<uint8_t, 4096> payload{};
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    REQUIRE(stream.write(payload.data(), payload.size(), false) == static_cast<int32_t>(payload.size()));

    stream.m_nextSendOffset = 3072;
    stream.m_sendAckedOffset = 0;
    stream.m_sendInFlightBytes = 3072;
    stream.m_sendQueuedBytes = 1024;
    stream.m_localFinQueued = true;
    stream.m_localFinSent = false;

    REQUIRE(stream.flushPendingSends(2048) == UTP_ERR_OK);
    REQUIRE(stream.m_nextSendOffset == payload.size());
    REQUIRE(stream.m_sendQueuedBytes == 0);
    REQUIRE(stream.m_localFinSent);
}

TEST_CASE("ConnectionImpl: scheduler skips deferred streams", "[Connection][Stream][Coalescing]")
{
    Config cfg;
    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerDisabled;
    cfg.stream_enable_coalescing = true;
    cfg.stream_min_payload_before_immediate_send = 512;
    cfg.stream_coalesce_delay_us = 5000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1019);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;

    StreamImpl::SP deferred = std::make_shared<StreamImpl>(&conn, 4, 4);
    StreamImpl::SP ready = std::make_shared<StreamImpl>(&conn, 8, 4);
    const utp_time_t nowUs = eular::utp::time::MonotonicUs();

    deferred->m_sendQueuedBytes = 128;
    deferred->m_lastSendQueuedAtUs = nowUs - 500;
    ready->m_sendQueuedBytes = 128;
    ready->m_lastSendQueuedAtUs = nowUs - 6000;

    conn.m_streams.emplace(deferred->id(), deferred);
    conn.m_streams.emplace(ready->id(), ready);

    StreamImpl::SP selected = conn.pickNextWritableStream();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == ready->id());

    ready->m_lastSendQueuedAtUs = nowUs - 500;
    selected = conn.pickNextWritableStream();
    REQUIRE(selected == nullptr);
}
