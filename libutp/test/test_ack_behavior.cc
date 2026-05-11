/*************************************************************************
    > File Name: test_ack_behavior.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <event2/event.h>
#include <event/loop.h>
#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#include "context/send_ctl.h"
#undef private

#include "proto/proto.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/handshake_done.h"
#include "proto/frame/stream.h"
#include "utp/errno.h"
#include "util/ack_info.h"
#include "util/time.h"

namespace eular {
namespace utp {
static constexpr int32_t UTP_ERR_OK = ::UTP_ERR_OK;
} // namespace utp
} // namespace eular

using eular::Serialize;
using eular::utp::Config;
using eular::utp::ConnectionImpl;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::FrameAckFrequency;
using eular::utp::FrameHandshakeDone;
using eular::utp::FrameStream;
using eular::utp::FrameType;
using eular::utp::PacketOut;
using eular::utp::SendControl;
using eular::utp::Status;

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
    Status st;
    REQUIRE(ackFreq.encode(payload.data(), payload.size(), st) == ackFreq.frameSize());
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
    Status st;
    REQUIRE(frame.encode(payload.data(), payload.size(), st) == static_cast<int32_t>(payload.size()));
    return payload;
}

bool AcceptPending(ContextImpl &server)
{
    if (!server.m_pendingIncomingQueue.empty()) {
        return server.accept().ok();
    }
    return false;
}

utp_packno_t LargestUnackedPackNo(const SendControl *sendCtl)
{
    utp_packno_t maxPackNo = 0;
    if (sendCtl == nullptr) {
        return maxPackNo;
    }

    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno > maxPackNo) {
            maxPackNo = pkt->packno;
        }
    }
    return maxPackNo;
}

size_t CountUnackedPacketsAfterWithBits(const SendControl *sendCtl,
                                        utp_packno_t afterPackNo,
                                        uint32_t frameBits,
                                        bool requireAllBits = false)
{
    if (sendCtl == nullptr) {
        return 0;
    }

    size_t count = 0;
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }

        if (requireAllBits) {
            if ((pkt->frame_types & frameBits) == frameBits) {
                ++count;
            }
        } else if ((pkt->frame_types & frameBits) != 0) {
            ++count;
        }
    }
    return count;
}

size_t SumUnackedStreamBytesAfter(const SendControl *sendCtl, utp_packno_t afterPackNo)
{
    if (sendCtl == nullptr) {
        return 0;
    }

    size_t bytes = 0;
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }
        bytes += pkt->stream_data_size;
    }
    return bytes;
}

PacketOut *BuildLostPacket(ContextImpl &owner,
                           ConnectionImpl::SP conn,
                           const std::vector<uint8_t> &payload,
                           const std::vector<eular::utp::FrameMetaInfo> &metas,
                           uint32_t frameTypes,
                           uint32_t streamDataBytes,
                           uint16_t transientAckBytes)
{
    REQUIRE(conn != nullptr);

    const std::vector<uint8_t> wire = BuildWirePacket(conn->cid(),
                                                      conn->m_peerConnectionID,
                                                      1,
                                                      UTP_TYPE_CTRL,
                                                      payload);
    PacketOut *pkt = owner.m_mm.getPacketOut(static_cast<uint32_t>(wire.size()));
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->raw_data != nullptr);
    REQUIRE(pkt->alloc_size >= wire.size());

    std::memcpy(pkt->raw_data, wire.data(), wire.size());
    pkt->data_size = static_cast<uint16_t>(wire.size());
    pkt->frame_types = frameTypes;
    pkt->stream_data_size = streamDataBytes;
    pkt->transient_ack_size = transientAckBytes;
    pkt->frame_meta_count = static_cast<uint8_t>(metas.size());
    REQUIRE(metas.size() <= eular::utp::PACKET_OUT_MAX_FRAMES);

    for (size_t i = 0; i < metas.size(); ++i) {
        pkt->frame_meta[i] = metas[i];
    }

    pkt->po_flags |= eular::utp::PacketOutFlags::kPoLost;
    pkt->po_flags |= eular::utp::PacketOutFlags::kPoLossRecorded;
    pkt->po_flags |= eular::utp::PacketOutFlags::kPoResetPackNo;
    TAILQ_INSERT_TAIL(&conn->m_sendCtl->m_lostPackets, pkt, po_next);
    return pkt;
}

PacketOut *BuildUnackedPacket(ConnectionImpl &conn,
                              utp_packno_t packno,
                              uint32_t frameTypes,
                              uint32_t streamDataBytes = 0,
                              uint16_t transientAckBytes = 0,
                              uint16_t payloadBytes = 1)
{
    const uint32_t packetSize = static_cast<uint32_t>(UTP_HEADER_SIZE + payloadBytes);
    PacketOut *pkt = conn.m_mm.getPacketOut(packetSize);
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->raw_data != nullptr);
    REQUIRE(pkt->alloc_size >= packetSize);

    std::memset(pkt->raw_data, 0, packetSize);
    pkt->data_size = static_cast<uint16_t>(packetSize);
    pkt->packno = packno;
    pkt->sent_time = eular::utp::time::MonotonicUs() - 1000;
    pkt->frame_types = frameTypes;
    pkt->stream_data_size = streamDataBytes;
    pkt->transient_ack_size = transientAckBytes;
    conn.m_sendCtl->appendUnacked(pkt);
    return pkt;
}

eular::utp::AckInfo BuildAckRange(utp_packno_t low, utp_packno_t high, utp_time_t ackDelayUs = 0)
{
    eular::utp::AckInfo ackInfo;
    ackInfo.largest_ack_packno = high;
    ackInfo.ack_delay = ackDelayUs;
    ackInfo.range_size = 1;
    ackInfo.ack_ranges[0].low = low;
    ackInfo.ack_ranges[0].high = high;
    return ackInfo;
}

eular::utp::AckInfo BuildAckRanges(std::initializer_list<Range> ranges, utp_time_t ackDelayUs = 0)
{
    eular::utp::AckInfo ackInfo;
    ackInfo.ack_delay = ackDelayUs;
    ackInfo.range_size = static_cast<uint32_t>(ranges.size());
    REQUIRE(ackInfo.range_size > 0);
    REQUIRE(ackInfo.range_size <= ackInfo.ack_ranges.size());

    utp_packno_t largest = 0;
    size_t index = 0;
    for (const auto &range : ranges) {
        ackInfo.ack_ranges[index++] = range;
        if (range.high > largest) {
            largest = range.high;
        }
    }
    ackInfo.largest_ack_packno = largest;
    return ackInfo;
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

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

    const int32_t sid = clientConn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
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

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

    clientConn->m_lastAckFrequencyApplyMs = 0;
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

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

    const int32_t serverSid = serverConn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
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

    const int32_t clientSid = clientConn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
    REQUIRE(clientSid >= 0);
    auto clientStreamIt = clientConn->m_streams.find(static_cast<uint32_t>(clientSid));
    REQUIRE(clientStreamIt != clientConn->m_streams.end());
    REQUIRE(clientStreamIt->second != nullptr);
    REQUIRE(clientStreamIt->second->write("cli", 3, false) == 3);

    const uint32_t ackStreamBits = (1u << static_cast<uint32_t>(FrameType::kFrameAck))
                                 | (1u << static_cast<uint32_t>(FrameType::kFrameStream));
    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return HasUnackedPacketWithBits(clientConn->m_sendCtl.get(), ackStreamBits);
        },
        nullptr,
        200,
        1));
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

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

    clientConn->m_lastAckFrequencyApplyMs = 0;
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

TEST_CASE("Passive handshake inherits peer AckFrequency from pending initial parsing", "[Ack][Integration][Passive]")
{
    Config serverCfg;
    serverCfg.handshake_timeout = 200;
    serverCfg.ack_every_n_packets = 10;
    serverCfg.ack_delay = 200;

    Config clientCfg;
    clientCfg.handshake_timeout = 200;
    clientCfg.ack_every_n_packets = 50;
    clientCfg.ack_delay = 77;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &serverCfg);
    ContextImpl client(loop.loop(), &clientCfg);

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) { return true; });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(serverConn != nullptr);
    REQUIRE(serverConn->m_ackElicitingThreshold == 50);
    REQUIRE(serverConn->m_ackReorderingThreshold == FrameAckFrequency::kDefaultReorderingThreshold);
    REQUIRE(serverConn->m_ackMaxDelayMs == 77);
}

TEST_CASE("Adaptive AckFrequency updates on sustained RTT increase", "[Ack][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "").ok());
    REQUIRE(client.bind("127.0.0.1", 0, "").ok());

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info).ok());

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

TEST_CASE("Ack without HandshakeDone coverage keeps HandshakeDone pending", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2101);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));
    BuildUnackedPacket(conn, 10, (1u << static_cast<uint32_t>(FrameType::kFrameStream)), 16);

    const Status st = conn.m_sendCtl->onAckReceived(BuildAckRange(10, 10), eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE(conn.m_handshakeDonePending);
    REQUIRE(conn.m_handshakeDoneTimer.isActive());
    REQUIRE(TAILQ_EMPTY(&conn.m_sendCtl->m_unackedPackets));
}

TEST_CASE("Ack covering HandshakeDone packet clears pending convergence", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2102);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));
    const uint32_t handshakeDoneBits = (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone));
    BuildUnackedPacket(conn, 20, handshakeDoneBits);

    const Status st = conn.m_sendCtl->onAckReceived(BuildAckRange(20, 20), eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE_FALSE(conn.m_handshakeDonePending);
    REQUIRE_FALSE(conn.m_handshakeDoneTimer.isActive());
    REQUIRE(TAILQ_EMPTY(&conn.m_sendCtl->m_unackedPackets));
}

TEST_CASE("Ack that skips HandshakeDone packet does not clear pending convergence", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2103);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));

    const uint32_t handshakeDoneBits = (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone));
    const uint32_t streamBits = (1u << static_cast<uint32_t>(FrameType::kFrameStream));
    BuildUnackedPacket(conn, 30, handshakeDoneBits);
    BuildUnackedPacket(conn, 31, streamBits, 8);

    const Status st = conn.m_sendCtl->onAckReceived(BuildAckRange(31, 31), eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE(conn.m_handshakeDonePending);
    REQUIRE(conn.m_handshakeDoneTimer.isActive());
    REQUIRE_FALSE(HasUnackedPacketWithBits(conn.m_sendCtl.get(), streamBits));
}

TEST_CASE("Ack of packet carrying Ack Stream and HandshakeDone clears pending", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2104);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));

    const uint32_t frameBits = (1u << static_cast<uint32_t>(FrameType::kFrameAck))
                             | (1u << static_cast<uint32_t>(FrameType::kFrameStream))
                             | (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone));
    BuildUnackedPacket(conn, 40, frameBits, 32, 12);

    const Status st = conn.m_sendCtl->onAckReceived(BuildAckRange(40, 40), eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE_FALSE(conn.m_handshakeDonePending);
    REQUIRE_FALSE(conn.m_handshakeDoneTimer.isActive());
    REQUIRE(TAILQ_EMPTY(&conn.m_sendCtl->m_unackedPackets));
}

TEST_CASE("Sparse ACK ranges that do not cover HandshakeDone keep pending convergence", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2105);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));

    const uint32_t handshakeDoneBits = (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone));
    const uint32_t streamBits = (1u << static_cast<uint32_t>(FrameType::kFrameStream));
    BuildUnackedPacket(conn, 50, handshakeDoneBits);
    BuildUnackedPacket(conn, 51, streamBits, 8);
    BuildUnackedPacket(conn, 53, streamBits, 8);

    const Status st = conn.m_sendCtl->onAckReceived(
        BuildAckRanges({Range{51, 51}, Range{53, 53}}),
        eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE(conn.m_handshakeDonePending);
    REQUIRE(conn.m_handshakeDoneTimer.isActive());
}

TEST_CASE("Sparse ACK ranges clear pending once one range covers HandshakeDone", "[Ack][HandshakeDone]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, nullptr, 2106);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_handshakeDonePending = true;
    REQUIRE(conn.m_handshakeDoneTimer.start(1000));

    const uint32_t handshakeDoneBits = (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone));
    const uint32_t streamBits = (1u << static_cast<uint32_t>(FrameType::kFrameStream));
    BuildUnackedPacket(conn, 60, handshakeDoneBits);
    BuildUnackedPacket(conn, 61, streamBits, 8);
    BuildUnackedPacket(conn, 63, streamBits, 8);

    const Status st = conn.m_sendCtl->onAckReceived(
        BuildAckRanges({Range{61, 61}, Range{60, 60}, Range{63, 63}}),
        eular::utp::time::MonotonicUs());
    REQUIRE(st.ok());
    REQUIRE_FALSE(conn.m_handshakeDonePending);
    REQUIRE_FALSE(conn.m_handshakeDoneTimer.isActive());
}
