/*************************************************************************
    > File Name: test_ack_behavior.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

#include <event2/event.h>
#include <event/loop.h>
#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#undef private

#include "context/send_ctl.h"
#include "proto/proto.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/stream.h"
#include "util/time.h"

using eular::Serialize;
using eular::utp::Config;
using eular::utp::ConnectionImpl;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::FrameAckFrequency;
using eular::utp::FrameStream;
using eular::utp::FrameType;

namespace {

bool PumpUntil(ev::EventLoop &loop,
               const std::function<bool()> &done,
               const std::function<void()> &tick,
               int32_t maxRounds = 200,
               int32_t sleepMs = 1)
{
    for (int32_t i = 0; i < maxRounds; ++i) {
        if (tick) {
            tick();
        }
        if (done()) {
            return true;
        }
        loop.dispatch(EVLOOP_NONBLOCK | EVLOOP_ONCE);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return done();
}

uint16_t BoundPort(const ContextImpl &ctx)
{
    return ctx.m_udpSocket.m_localAddr.port();
}

ConnectionImpl::SP FindConnectedByRemote(const ContextImpl &ctx, uint16_t port)
{
    for (const auto &entry : ctx.m_connections) {
        if (entry.second
            && entry.second->connectInfo().port == port
            && entry.second->state() == ConnectionImpl::kStateConnected) {
            return entry.second;
        }
    }
    return ConnectionImpl::SP();
}

bool HasUnackedPacketWithBits(const eular::utp::SendControl *sendCtl, uint32_t bits)
{
    if (sendCtl == nullptr) {
        return false;
    }

    eular::utp::PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if ((pkt->frame_types & bits) == bits) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> BuildWirePacket(uint32_t scid,
                                     uint32_t dcid,
                                     uint64_t pn,
                                     uint8_t packetType,
                                     const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> wire(UTP_HEADER_SIZE + payload.size(), 0);
    uint8_t *offset = wire.data();
    size_t left = wire.size();

    offset = Serialize::SerializeTo(offset, left, scid);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, dcid);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, pn);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(payload.size()));
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, packetType);
    REQUIRE(offset != nullptr);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    REQUIRE(offset != nullptr);

    if (!payload.empty()) {
        std::memcpy(offset, payload.data(), payload.size());
    }

    return wire;
}

std::vector<uint8_t> BuildAckFrequencyPayload(uint8_t ackThreshold,
                                              uint8_t reorderThreshold,
                                              uint32_t maxAckDelayMs)
{
    FrameAckFrequency ackFreq;
    ackFreq.ack_eliciting_threshold = ackThreshold;
    ackFreq.reordering_threshold = reorderThreshold;
    ackFreq.max_ack_delay_ms = maxAckDelayMs;

    std::vector<uint8_t> payload(static_cast<size_t>(ackFreq.frameSize()), 0);
    REQUIRE(ackFreq.encode(payload.data(), payload.size()) == ackFreq.frameSize());
    return payload;
}

std::vector<uint8_t> BuildStreamPayload(uint32_t streamId,
                                        uint64_t streamOffset,
                                        const std::string &data)
{
    FrameStream frame;
    frame.stream_id = streamId;
    frame.stream_offset = streamOffset;
    frame.stream_data_length = static_cast<uint16_t>(data.size());
    frame.stream_data = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(data.data()));

    std::vector<uint8_t> payload(static_cast<size_t>(FRAME_STREAM_HDR_SIZE) + data.size(), 0);
    REQUIRE(frame.encode(payload.data(), payload.size()) == static_cast<int32_t>(payload.size()));
    return payload;
}

bool AcceptPending(ContextImpl &server)
{
    if (!server.m_pendingIncomingQueue.empty()) {
        return server.accept() == eular::utp::UTP_ERR_OK;
    }
    return false;
}

} // namespace

TEST_CASE("Ack timer sends delayed ACK without follow-up packets", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;
    cfg.ack_every_n_packets = 10;
    cfg.ack_delay = 20;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == eular::utp::UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            (void)AcceptPending(server);
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(clientConn != nullptr);
    REQUIRE(serverConn != nullptr);

    const int32_t sid = clientConn->createStream();
    REQUIRE(sid >= 0);
    auto clientStreamIt = clientConn->m_streams.find(static_cast<uint32_t>(sid));
    REQUIRE(clientStreamIt != clientConn->m_streams.end());
    REQUIRE(clientStreamIt->second != nullptr);
    REQUIRE(clientStreamIt->second->write("ping", 4, false) == 4);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return TAILQ_EMPTY(&clientConn->m_sendCtl->m_unackedPackets);
        },
        nullptr,
        400,
        1));

    REQUIRE(TAILQ_EMPTY(&clientConn->m_sendCtl->m_unackedPackets));
    REQUIRE(serverConn->m_ackElicitingSinceLastAck == 0);
}

TEST_CASE("Ack reordering threshold updates send side and triggers immediate ACK on gap", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;
    cfg.ack_every_n_packets = 10;
    cfg.ack_delay = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == eular::utp::UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            (void)AcceptPending(server);
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(clientConn != nullptr);
    REQUIRE(serverConn != nullptr);

    const std::vector<uint8_t> ackFreqPayload = BuildAckFrequencyPayload(10, 1, 200);
    const std::vector<uint8_t> ackFreqWire = BuildWirePacket(serverConn->cid(),
                                                             clientConn->cid(),
                                                             clientConn->m_receiveHistory.largest() + 1,
                                                             UTP_TYPE_CTRL,
                                                             ackFreqPayload);

    eular::utp::UdpSocket::MsgMetaInfo ackFreqMsg{};
    ackFreqMsg.data = ackFreqWire.data();
    ackFreqMsg.len = ackFreqWire.size();
    ackFreqMsg.metaInfo.peerAddress = server.m_udpSocket.m_localAddr;
    clientConn->onUdpPacket(ackFreqMsg);

    REQUIRE(clientConn->m_sendCtl->m_reorderThresh == 1);
    clientConn->m_ackElicitingSinceLastAck = 0;
    clientConn->m_ackPendingSinceUs = 0;
    clientConn->m_ackTimer.stop();

    const uint64_t bytesOutBefore = clientConn->m_bytesOut;
    const std::vector<uint8_t> streamPayload = BuildStreamPayload(77, 0, "gap");
    const std::vector<uint8_t> streamWire = BuildWirePacket(serverConn->cid(),
                                                            clientConn->cid(),
                                                            clientConn->m_receiveHistory.largest() + 3,
                                                            UTP_TYPE_CTRL,
                                                            streamPayload);

    eular::utp::UdpSocket::MsgMetaInfo streamMsg{};
    streamMsg.data = streamWire.data();
    streamMsg.len = streamWire.size();
    streamMsg.metaInfo.peerAddress = server.m_udpSocket.m_localAddr;
    clientConn->onUdpPacket(streamMsg);

    REQUIRE(clientConn->m_ackElicitingSinceLastAck == 0);
    REQUIRE_FALSE(clientConn->m_ackTimer.isActive());
    REQUIRE(clientConn->m_bytesOut > bytesOutBefore);
}

TEST_CASE("Ack piggyback coalesces pending ACK with outgoing stream packet", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;
    cfg.ack_every_n_packets = 10;
    cfg.ack_delay = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == eular::utp::UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            (void)AcceptPending(server);
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(clientConn != nullptr);
    REQUIRE(serverConn != nullptr);

    const int32_t serverSid = serverConn->createStream();
    REQUIRE(serverSid >= 0);
    auto serverStreamIt = serverConn->m_streams.find(static_cast<uint32_t>(serverSid));
    REQUIRE(serverStreamIt != serverConn->m_streams.end());
    REQUIRE(serverStreamIt->second != nullptr);
    REQUIRE(serverStreamIt->second->write("srv", 3, false) == 3);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return clientConn->m_ackElicitingSinceLastAck > 0;
        },
        nullptr,
        200,
        1));

    const int32_t clientSid = clientConn->createStream();
    REQUIRE(clientSid >= 0);
    auto clientStreamIt = clientConn->m_streams.find(static_cast<uint32_t>(clientSid));
    REQUIRE(clientStreamIt != clientConn->m_streams.end());
    REQUIRE(clientStreamIt->second != nullptr);
    REQUIRE(clientStreamIt->second->write("cli", 3, false) == 3);

    const uint32_t ackStreamBits = (1u << static_cast<uint32_t>(FrameType::kFrameAck))
                                 | (1u << static_cast<uint32_t>(FrameType::kFrameStream));
    REQUIRE(HasUnackedPacketWithBits(clientConn->m_sendCtl.get(), ackStreamBits));
    REQUIRE(clientConn->m_ackElicitingSinceLastAck == 0);
    REQUIRE_FALSE(clientConn->m_ackTimer.isActive());
}

TEST_CASE("AckFrequency update is throttled within minimum interval", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;
    cfg.ack_every_n_packets = 10;
    cfg.ack_delay = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == eular::utp::UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            (void)AcceptPending(server);
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(clientConn != nullptr);
    REQUIRE(serverConn != nullptr);

    const std::vector<uint8_t> firstAckFreqPayload = BuildAckFrequencyPayload(20, 7, 120);
    const std::vector<uint8_t> firstAckFreqWire = BuildWirePacket(serverConn->cid(),
                                                                  clientConn->cid(),
                                                                  clientConn->m_receiveHistory.largest() + 1,
                                                                  UTP_TYPE_CTRL,
                                                                  firstAckFreqPayload);
    eular::utp::UdpSocket::MsgMetaInfo firstMsg{};
    firstMsg.data = firstAckFreqWire.data();
    firstMsg.len = firstAckFreqWire.size();
    firstMsg.metaInfo.peerAddress = server.m_udpSocket.m_localAddr;
    clientConn->onUdpPacket(firstMsg);

    REQUIRE(clientConn->m_ackElicitingThreshold == 20);
    REQUIRE(clientConn->m_ackReorderingThreshold == 7);
    REQUIRE(clientConn->m_ackMaxDelayMs == 120);

    const std::vector<uint8_t> secondAckFreqPayload = BuildAckFrequencyPayload(2, 1, 10);
    const std::vector<uint8_t> secondAckFreqWire = BuildWirePacket(serverConn->cid(),
                                                                   clientConn->cid(),
                                                                   clientConn->m_receiveHistory.largest() + 2,
                                                                   UTP_TYPE_CTRL,
                                                                   secondAckFreqPayload);
    eular::utp::UdpSocket::MsgMetaInfo secondMsg{};
    secondMsg.data = secondAckFreqWire.data();
    secondMsg.len = secondAckFreqWire.size();
    secondMsg.metaInfo.peerAddress = server.m_udpSocket.m_localAddr;
    clientConn->onUdpPacket(secondMsg);

    // 第二次更新与第一次间隔过短，应该被节流忽略。
    REQUIRE(clientConn->m_ackElicitingThreshold == 20);
    REQUIRE(clientConn->m_ackReorderingThreshold == 7);
    REQUIRE(clientConn->m_ackMaxDelayMs == 120);
}

TEST_CASE("Adaptive AckFrequency updates on sustained RTT increase", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == eular::utp::UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == eular::utp::UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            (void)AcceptPending(server);
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    REQUIRE(clientConn != nullptr);

    const utp_time_t nowUs = eular::utp::time::MonotonicUs();
    clientConn->m_ackProfileCurrent = ConnectionImpl::kAckProfileStable;
    clientConn->m_ackProfileCandidate = ConnectionImpl::kAckProfileStable;
    clientConn->m_ackProfileCandidateSinceUs = 0;
    clientConn->m_ackProfileLastSentMs = 0;
    clientConn->m_ackProfileBaselineSrttUs = 100000;
    clientConn->m_rttStats.update(20000);

    clientConn->maybeUpdateAckFrequency(nowUs);
    REQUIRE(clientConn->m_ackProfileCandidate == ConnectionImpl::kAckProfileLatencySensitive);

    clientConn->m_ackProfileCandidateSinceUs = nowUs - 4000000;
    clientConn->maybeUpdateAckFrequency(nowUs);

    REQUIRE(clientConn->m_ackProfileCurrent == ConnectionImpl::kAckProfileLatencySensitive);
    const uint32_t ackFrequencyBits = (1u << static_cast<uint32_t>(FrameType::kFrameAckFrequency));
    REQUIRE(HasUnackedPacketWithBits(clientConn->m_sendCtl.get(), ackFrequencyBits));
}
