/*************************************************************************
    > File Name: test_connection_stream_layer.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include <event/loop.h>
#include <event2/event.h>

#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#include "context/send_ctl.h"
#include "context/stream_impl.h"
#undef private

#include "utp/errno.h"
#include "proto/proto.h"
#include "proto/packet_out.h"
#include "util/time.h"

using eular::utp::Config;
using eular::utp::Connection;
using eular::utp::ConnectionImpl;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::FrameStream;
using eular::utp::Stream;
using eular::utp::StreamImpl;
using eular::utp::Address;
using eular::utp::UdpSocket;
using eular::utp::Status;

namespace {

std::vector<uint8_t> BuildRawPacket(uint32_t scid,
                                    uint32_t dcid,
                                    uint64_t pn,
                                    uint8_t packetType,
                                    const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> packet(static_cast<size_t>(UTP_HEADER_SIZE) + payload.size(), 0);
    uint8_t *offset = packet.data();
    size_t left = packet.size();

    offset = eular::Serialize::SerializeTo(offset, left, scid);
    offset = eular::Serialize::SerializeTo(offset, left, dcid);
    offset = eular::Serialize::SerializeTo(offset, left, pn);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint16_t>(payload.size()));
    offset = eular::Serialize::SerializeTo(offset, left, packetType);
    offset = eular::Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);
    REQUIRE(left == payload.size());

    if (!payload.empty()) {
        std::memcpy(offset, payload.data(), payload.size());
    }
    return packet;
}

eular::utp::PacketOut *LastUnackedPacket(eular::utp::SendControl *sendCtl)
{
    if (sendCtl == nullptr) {
        return nullptr;
    }

    eular::utp::PacketOut *last = nullptr;
    eular::utp::PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        last = pkt;
    }
    return last;
}

eular::utp::PacketOut *FirstScheduledPacket(eular::utp::SendControl *sendCtl)
{
    return sendCtl == nullptr ? nullptr : TAILQ_FIRST(&sendCtl->m_scheduledPackets);
}

eular::utp::PacketOut *FindUnackedPacketWithBits(eular::utp::SendControl *sendCtl, uint32_t bits)
{
    if (sendCtl == nullptr) {
        return nullptr;
    }

    eular::utp::PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if ((pkt->frame_types & bits) == bits) {
            return pkt;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("ConnectionImpl: getStream returns created stream pointer", "[Connection][Stream]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1001);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerTP.init_max_streams_bidi = 8;

    const int32_t streamId = conn.createStream(Connection::kStreamTypeBidirectional);
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

    const int32_t localStreamId = conn.createStream(Connection::kStreamTypeBidirectional);
    REQUIRE(localStreamId == 0);
    REQUIRE(callbackCount == 0);

    FrameStream incoming;
    uint8_t byte = 7;
    incoming.stream_id = 1;
    incoming.stream_offset = 0;
    incoming.stream_data_length = 1;
    incoming.stream_data = &byte;
    REQUIRE(conn.ingestStreamFrame(incoming).ok());
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

TEST_CASE("ConnectionImpl: handshake done delay uses min of local and peer timeout", "[Connection][Handshake]")
{
    Config cfg;
    cfg.handshake_timeout = 3000;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1100);
    conn.m_connectInfo.timeout = 4000;
    conn.m_peerTP.handshake_timeout = 6000;
    REQUIRE(conn.handshakeDoneDelayMs() == (4000u / 3u));

    conn.m_peerTP.handshake_timeout = 900;
    REQUIRE(conn.handshakeDoneDelayMs() == 300);
}

TEST_CASE("ConnectionImpl: active handshake timeout uses exponential backoff and fails after retries", "[Connection][Handshake]")
{
    Config cfg;
    cfg.handshake_timeout = 10;
    cfg.handshake_max_retries = 2;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1101);
    conn.m_state = ConnectionImpl::kStateInitialSent;
    conn.m_connectInfo.timeout = 10;

    REQUIRE(conn.handshakeTimeoutForRoundMs(0, false) == 10);
    REQUIRE(conn.handshakeTimeoutForRoundMs(1, false) == 20);
    REQUIRE(conn.handshakeTimeoutForRoundMs(2, false) == 40);

    conn.m_handshakeRetryCount = cfg.handshake_max_retries;
    conn.onConnTimeout();
    REQUIRE(conn.state() == ConnectionImpl::kStateDisconnected);
    REQUIRE(conn.lastErrorCode() == UTP_ERR_TIMEOUT);
}

TEST_CASE("ConnectionImpl: timeout callback after handshake promotion does not disconnect connection",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    cfg.handshake_timeout = 10;
    cfg.handshake_max_retries = 2;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1102);
    conn.m_state = ConnectionImpl::kStateInitialSent;

    const std::vector<uint8_t> payload;
    const std::vector<uint8_t> packetBytes = BuildRawPacket(0x22334455, conn.cid(), 7, UTP_TYPE_HANDSHAKE, payload);
    UdpSocket::MsgMetaInfo msg{};
    msg.data = (void *)packetBytes.data();
    msg.len = packetBytes.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 23456);

    conn.onUdpPacket(msg);
    REQUIRE(conn.state() == ConnectionImpl::kStateConnected);
    REQUIRE(conn.m_handshakeDonePending);

    conn.onConnTimeout();
    REQUIRE(conn.state() == ConnectionImpl::kStateConnected);
    REQUIRE(conn.m_handshakeDonePending);
    REQUIRE(conn.lastErrorCode() == UTP_ERR_OK);
}

TEST_CASE("ConnectionImpl: late handshake packet does not revive timed out connection",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    cfg.handshake_timeout = 10;
    cfg.handshake_max_retries = 0;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1103);
    conn.m_state = ConnectionImpl::kStateInitialSent;
    conn.m_connectInfo.timeout = 10;

    conn.onConnTimeout();
    REQUIRE(conn.state() == ConnectionImpl::kStateDisconnected);
    REQUIRE(conn.lastErrorCode() == UTP_ERR_TIMEOUT);

    const std::vector<uint8_t> payload;
    const std::vector<uint8_t> packetBytes = BuildRawPacket(0x55667788, conn.cid(), 9, UTP_TYPE_HANDSHAKE, payload);
    UdpSocket::MsgMetaInfo msg{};
    msg.data = (void *)packetBytes.data();
    msg.len = packetBytes.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 23457);

    conn.onUdpPacket(msg);
    REQUIRE(conn.state() == ConnectionImpl::kStateDisconnected);
    REQUIRE(conn.lastErrorCode() == UTP_ERR_TIMEOUT);
}

TEST_CASE("ConnectionImpl: encrypted handshake cannot silently downgrade to plaintext",
          "[Connection][Handshake][Crypto][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 11031);
    conn.m_state = ConnectionImpl::kStateInitialSent;
    conn.m_connectInfo.encrypted = Context::kEncryptionAesGcm128;

    const std::vector<uint8_t> payload;
    const std::vector<uint8_t> packetBytes =
        BuildRawPacket(0x66778899, conn.cid(), 9, UTP_TYPE_HANDSHAKE, payload);
    UdpSocket::MsgMetaInfo msg{};
    msg.data = (void *)packetBytes.data();
    msg.len = packetBytes.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 23459);

    conn.onUdpPacket(msg);
    REQUIRE(conn.state() == ConnectionImpl::kStateCloseSent);
    REQUIRE(conn.lastErrorCode() == UTP_ERR_CRYPTO_INIT_FAILED);
    REQUIRE(conn.m_txAesCtx == nullptr);
    REQUIRE(conn.m_rxAesCtx == nullptr);
}

TEST_CASE("ConnectionImpl: duplicate handshake while connected rearms HandshakeDone convergence",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1104);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = false;
    conn.m_peerHandshakePacketNo = 0;
    conn.m_handshakeReceivedAtUs = 0;

    const std::vector<uint8_t> payload;
    const std::vector<uint8_t> packetBytes = BuildRawPacket(0x8899aabb, conn.cid(), 15, UTP_TYPE_HANDSHAKE, payload);
    UdpSocket::MsgMetaInfo msg{};
    msg.data = (void *)packetBytes.data();
    msg.len = packetBytes.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 23458);

    conn.onUdpPacket(msg);
    REQUIRE(conn.state() == ConnectionImpl::kStateConnected);
    REQUIRE(conn.m_handshakeDonePending);
    REQUIRE(conn.m_peerHandshakePacketNo == 15);
    REQUIRE(conn.m_handshakeReceivedAtUs > 0);
}

TEST_CASE("ConnectionImpl: stream data packet piggybacks HandshakeDone while pending",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1105);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;

    REQUIRE(conn.shouldPiggybackHandshakeDone(2, false));
    REQUIRE_FALSE(conn.shouldPiggybackHandshakeDone(0, false));
}

TEST_CASE("ConnectionImpl: FIN-only stream packet piggybacks HandshakeDone while pending",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 1106);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;

    REQUIRE(conn.shouldPiggybackHandshakeDone(0, true));
    REQUIRE_FALSE(conn.shouldPiggybackHandshakeDone(0, false));
}

TEST_CASE("ConnectionImpl: FIN-only stream send emits HandshakeDone metadata on wire packet",
          "[Connection][Handshake][Regression]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    UdpSocket sock(cfg);
    sock.m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock.m_sock == INVALID_SOCKET) {
        WARN("socket creation unavailable in current environment");
        return;
    }

    ConnectionImpl conn(&ctx, &sock, 1107);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerConnectionID = 2207;
    conn.m_peerAddress = Address("127.0.0.1", 9);
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_bytesIn = 4096;
    conn.m_handshakeDonePending = true;
    conn.m_peerTP.init_max_streams_bidi = 8;

    REQUIRE(conn.sendStreamFrame(0, 0, nullptr, 0, true).ok());

    REQUIRE(conn.m_bytesOut == 0);
    REQUIRE(conn.m_sendCtl->scheduledCount() == 1);
    REQUIRE(LastUnackedPacket(conn.m_sendCtl.get()) == nullptr);
    eular::utp::PacketOut *scheduled = FirstScheduledPacket(conn.m_sendCtl.get());
    REQUIRE(scheduled != nullptr);
    REQUIRE((scheduled->frame_types & (1u << static_cast<uint32_t>(eular::utp::kFrameStream))) != 0);

    conn.m_sendCtl->onCanWrite(eular::utp::time::MonotonicUs());

    const uint32_t expectedBits = (1u << static_cast<uint32_t>(eular::utp::kFrameStream)) |
                                  (1u << static_cast<uint32_t>(eular::utp::kFrameHandshakeDone)) |
                                  (1u << static_cast<uint32_t>(eular::utp::kFrameHandshakeDelay));
    eular::utp::PacketOut *pkt = FindUnackedPacketWithBits(conn.m_sendCtl.get(), expectedBits);
    REQUIRE(pkt != nullptr);
    REQUIRE((pkt->frame_types & (1u << static_cast<uint32_t>(eular::utp::kFrameStream))) != 0);
    REQUIRE((pkt->frame_types & (1u << static_cast<uint32_t>(eular::utp::kFrameHandshakeDone))) != 0);
    REQUIRE((pkt->frame_types & (1u << static_cast<uint32_t>(eular::utp::kFrameHandshakeDelay))) != 0);
    REQUIRE(pkt->frame_meta_count == 2);
    REQUIRE(pkt->frame_meta[0].frame_type == eular::utp::kFrameStream);
    REQUIRE(pkt->frame_meta[1].frame_type == eular::utp::kFrameHandshakeDone);
    REQUIRE(conn.m_handshakeDoneLastPacketNo == pkt->packno);
    REQUIRE(conn.m_sendCtl->scheduledCount() == 0);
    REQUIRE(conn.m_bytesOut > 0);
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
    REQUIRE(conn.ingestStreamFrame(valid).ok());

    FrameStream wrongRole = valid;
    wrongRole.stream_id = 0; // client-initiated, should not appear as ingress for client side
    REQUIRE(conn.ingestStreamFrame(wrongRole).code() == UTP_ERR_STREAM_STATE_ERROR);

    FrameStream validUni = valid;
    validUni.stream_id = 3; // server-initiated uni stream
    REQUIRE(conn.ingestStreamFrame(validUni).ok());

    FrameStream overLimit = valid;
    overLimit.stream_id = 5; // second server-initiated bidi stream (ordinal=2)
    REQUIRE(conn.ingestStreamFrame(overLimit).code() == UTP_ERR_STREAM_LIMIT_ERROR);

    FrameStream overLimitUni = valid;
    overLimitUni.stream_id = 7; // second server-initiated uni stream (ordinal=2)
    REQUIRE(conn.ingestStreamFrame(overLimitUni).code() == UTP_ERR_STREAM_LIMIT_ERROR);
}

TEST_CASE("ConnectionImpl: ingress enforces stream and connection flow control", "[Connection][Stream][FlowControl]")
{
    Config cfg;
    cfg.initial_max_data = 5;
    cfg.initial_max_stream_data_bidi_remote = 4;
    cfg.init_max_streams_bidi = 2;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ConnectionImpl conn(&ctx, nullptr, 10031);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_isClientInitiator = true;
    conn.m_loaclTP.init_max_streams_bidi = 2;
    conn.m_peerConnectionID = 20031;

    uint8_t bytes[3] = {1, 2, 3};
    FrameStream first;
    first.stream_id = 1;
    first.stream_offset = 0;
    first.stream_data_length = 3;
    first.stream_data = bytes;
    REQUIRE(conn.ingestStreamFrame(first).ok());
    REQUIRE(conn.m_localBytesReceivedTotal == 3);

    REQUIRE(conn.ingestStreamFrame(first).ok());
    REQUIRE(conn.m_localBytesReceivedTotal == 3);

    FrameStream streamOverflow = first;
    streamOverflow.stream_offset = 4;
    streamOverflow.stream_data_length = 1;
    REQUIRE(conn.ingestStreamFrame(streamOverflow).code() == UTP_ERR_STREAM_FLOW_CONTROL);

    FrameStream connectionOverflow = first;
    connectionOverflow.stream_id = 5;
    REQUIRE(conn.ingestStreamFrame(connectionOverflow).code() == UTP_ERR_STREAM_FLOW_CONTROL);
    REQUIRE(conn.m_streams.find(5) == conn.m_streams.end());

    FrameStream arithmeticOverflow = first;
    arithmeticOverflow.stream_offset = UINT64_MAX;
    arithmeticOverflow.stream_data_length = 2;
    REQUIRE(conn.ingestStreamFrame(arithmeticOverflow).code() == UTP_ERR_STREAM_FLOW_CONTROL);
}

TEST_CASE("ConnectionImpl: cwnd admission queues new stream data before UDP send",
          "[Connection][Stream][Congestion]")
{
    Config cfg;
    cfg.cc_algorithm = 2;
    cfg.cubic_init_cwnd_mss = 4;
    cfg.stream_unacked_data_limit = 1024 * 1024;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    UdpSocket sock(cfg);
    REQUIRE(sock.bind("127.0.0.1", 0, "").ok());

    ConnectionImpl conn(&ctx, &sock, 10032);
    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerConnectionID = 20032;
    conn.m_peerAddress = Address("127.0.0.1", 9);
    conn.m_networkPath.m_state = decltype(conn.m_networkPath)::kPathValidated;
    conn.m_peerMaxData = 1024 * 1024;
    conn.m_peerTP.initial_max_stream_data_bidi_local = 1024 * 1024;

    std::array<uint8_t, 1000> payload{};
    uint64_t offset = 0;
    size_t queued = 0;
    while (true) {
        const Status status = conn.sendStreamFrame(0, offset, payload.data(), payload.size(), false);
        if (!status.ok()) {
            REQUIRE(status.code() == UTP_ERR_WOULD_BLOCK);
            break;
        }
        offset += payload.size();
        ++queued;
    }

    REQUIRE(queued > 0);
    REQUIRE(conn.m_bytesOut == 0);
    REQUIRE(conn.m_sendCtl->scheduledCount() == queued);
    REQUIRE(conn.m_sendCtl->m_bytesScheduled <= conn.m_sendCtl->m_congestion->getCwnd());
    REQUIRE(TAILQ_EMPTY(&conn.m_sendCtl->m_unackedPackets));
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
        REQUIRE(conn.ingestStreamFrame(frame)  == 0);
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

    StreamImpl::SP selected = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sHigh->id());
    selected->m_sendQueuedBytes = 0;
    conn.updateStrictAgingState(selected->id());

    selected = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == sMid->id());
    selected->m_sendQueuedBytes = 0;
    conn.updateStrictAgingState(selected->id());

    selected = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
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

    StreamImpl::SP first = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
    REQUIRE(first != nullptr);
    REQUIRE(first->id() == sBase->id());
    conn.updateStrictAgingState(first->id());

    StreamImpl::SP second = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
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

    StreamImpl::SP first = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
    REQUIRE(first != nullptr);
    REQUIRE(first->id() == s1->id());
    first->m_sendQueuedBytes = 0;

    StreamImpl::SP second = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
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

    StreamImpl::SP selected = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == lowIdLowPrio->id());

    cfg.stream_scheduler_mode = eular::utp::kStreamSchedulerStrict;
    selected = conn.pickNextWritableStream(eular::utp::time::MonotonicUs());
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

    REQUIRE(stream.setPriority(0)  == 0);
    REQUIRE(stream.priority() == 0);
    REQUIRE(stream.setPriority(7)  == 0);
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

    REQUIRE(sock.bind("127.0.0.1", 0, "")  == 0);

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

    REQUIRE(stream.flushPendingSends(2048)  == 0);
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

    StreamImpl::SP selected = conn.pickNextWritableStream(nowUs);
    REQUIRE(selected != nullptr);
    REQUIRE(selected->id() == ready->id());

    ready->m_lastSendQueuedAtUs = nowUs - 500;
    selected = conn.pickNextWritableStream(nowUs);
    REQUIRE(selected == nullptr);
}
