/*************************************************************************
    > File Name: test_retrans_repack.cc
    > Author: eular
    > Brief:
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <event2/event.h>
#include <event/loop.h>

#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#include "context/send_ctl.h"
#undef private

#include "proto/proto.h"
#include "proto/frame/stream.h"
#include "proto/frame/handshake_done.h"
#include "utp/errno.h"
#include "util/time.h"

using eular::Serialize;
using eular::utp::Config;
using eular::utp::Connection;
using eular::utp::ConnectionImpl;
using eular::utp::Context;
using eular::utp::ContextImpl;
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
               int32_t maxRounds = 300,
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

bool AcceptPending(ContextImpl &server)
{
    if (!server.m_pendingIncomingQueue.empty()) {
        return server.accept()  == 0;
    }
    return false;
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

utp_packno_t LargestUnackedPackNo(const SendControl *sendCtl)
{
    utp_packno_t maxPackNo = 0;
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno > maxPackNo) {
            maxPackNo = pkt->packno;
        }
    }
    TAILQ_FOREACH(pkt, &sendCtl->m_scheduledPackets, po_next) {
        if (pkt->packno > maxPackNo) {
            maxPackNo = pkt->packno;
        }
    }
    return maxPackNo;
}

size_t CountUnackedPacketsAfterWithBits(const SendControl *sendCtl,
                                        utp_packno_t afterPackNo,
                                        uint32_t frameBits)
{
    size_t count = 0;
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }
        if ((pkt->frame_types & frameBits) != 0) {
            ++count;
        }
    }
    TAILQ_FOREACH(pkt, &sendCtl->m_scheduledPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }
        if ((pkt->frame_types & frameBits) != 0) {
            ++count;
        }
    }
    return count;
}

size_t SumUnackedStreamBytesAfter(const SendControl *sendCtl, utp_packno_t afterPackNo)
{
    size_t bytes = 0;
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH(pkt, &sendCtl->m_unackedPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }
        bytes += pkt->stream_data_size;
    }
    TAILQ_FOREACH(pkt, &sendCtl->m_scheduledPackets, po_next) {
        if (pkt->packno <= afterPackNo) {
            continue;
        }
        bytes += pkt->stream_data_size;
    }
    return bytes;
}

PacketOut *BuildLostPacket(ConnectionImpl::SP conn,
                           const std::vector<uint8_t> &payload,
                           const std::vector<eular::utp::FrameMetaInfo> &metas,
                           uint32_t frameTypes,
                           uint32_t streamDataBytes,
                           uint16_t transientAckBytes)
{
    const std::vector<uint8_t> wire = BuildWirePacket(conn->cid(),
                                                      conn->m_peerConnectionID,
                                                      1,
                                                      UTP_TYPE_CTRL,
                                                      payload);
    PacketOut *pkt = conn->m_mm.getPacketOut(static_cast<uint32_t>(wire.size()));
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

bool ConnectPair(ev::EventLoop &loop,
                 ContextImpl &server,
                 ContextImpl &client,
                 ConnectionImpl::SP &outClientConn,
                 ConnectionImpl::SP &outServerConn)
{
    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    if (client.connect(info)  != 0) {
        return false;
    }

    const bool ok = PumpUntil(loop,
                              [&]() {
                                  return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                                      && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
                              },
                              [&]() { (void)AcceptPending(server); },
                              300,
                              1);
    if (!ok) {
        return false;
    }

    outClientConn = FindConnectedByRemote(client, BoundPort(server));
    outServerConn = FindConnectedByRemote(server, BoundPort(client));
    return outClientConn != nullptr && outServerConn != nullptr;
}

} // namespace

TEST_CASE("Retrans repack: transient ACK frame is stripped while STREAM is preserved", "[Retrans][Repack]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "")  == 0);
    REQUIRE(client.bind("127.0.0.1", 0, "")  == 0);

    ConnectionImpl::SP clientConn;
    ConnectionImpl::SP serverConn;
    REQUIRE(ConnectPair(loop, server, client, clientConn, serverConn));

    clientConn->m_handshakeDonePending = false;
    clientConn->m_handshakeDoneSent = false;

    const std::vector<uint8_t> ackBytes = {static_cast<uint8_t>(FrameType::kFrameAck), 0x00, 0x00, 0x00};
    const std::string streamData = "repack-ack-strip";
    std::vector<uint8_t> streamBytes = BuildStreamPayload(77, 0, streamData);

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), ackBytes.begin(), ackBytes.end());
    payload.insert(payload.end(), streamBytes.begin(), streamBytes.end());

    std::vector<eular::utp::FrameMetaInfo> metas(2);
    metas[0].offset = UTP_HEADER_SIZE;
    metas[0].length = static_cast<uint16_t>(ackBytes.size());
    metas[0].frame_type = FrameType::kFrameAck;
    metas[0].frame_flags = static_cast<uint8_t>(eular::utp::kFMTransientOnRetrans | eular::utp::kFMDroppableOnMtu);
    metas[0].fmi_u.data = 0;
    metas[1].offset = static_cast<uint16_t>(UTP_HEADER_SIZE + ackBytes.size());
    metas[1].length = static_cast<uint16_t>(streamBytes.size());
    metas[1].frame_type = FrameType::kFrameStream;
    metas[1].frame_flags = static_cast<uint8_t>(eular::utp::kFMRetransMustKeep | eular::utp::kFMSplittable);
    metas[1].fmi_u.data = 0;

        PacketOut *lostPkt = BuildLostPacket(clientConn,
                                         payload,
                                         metas,
                                         (1u << static_cast<uint32_t>(FrameType::kFrameAck))
                                       | (1u << static_cast<uint32_t>(FrameType::kFrameStream)),
                                         streamData.size(),
                                         static_cast<uint16_t>(ackBytes.size()));

    const utp_packno_t beforePackNo = LargestUnackedPackNo(clientConn->m_sendCtl.get());
    REQUIRE(clientConn->m_sendCtl->retransmitSplitStreamPacket(lostPkt, eular::utp::time::MonotonicUs())
             == 0);

    REQUIRE(TAILQ_EMPTY(&clientConn->m_sendCtl->m_lostPackets));
    REQUIRE(CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(), beforePackNo,
                                             (1u << static_cast<uint32_t>(FrameType::kFrameAck))) == 0);
    REQUIRE(CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(), beforePackNo,
                                             (1u << static_cast<uint32_t>(FrameType::kFrameStream))) >= 1);
}

TEST_CASE("Retrans repack: large STREAM is split into multiple retrans packets", "[Retrans][Repack]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "")  == 0);
    REQUIRE(client.bind("127.0.0.1", 0, "")  == 0);

    ConnectionImpl::SP clientConn;
    ConnectionImpl::SP serverConn;
    REQUIRE(ConnectPair(loop, server, client, clientConn, serverConn));

    clientConn->m_handshakeDonePending = false;
    clientConn->m_handshakeDoneSent = false;
    clientConn->m_mtuDiscovery.m_currentMtu = ETHERNET_MTU_MIN;

    const std::string streamData(4096, 'x');
    std::vector<uint8_t> streamBytes = BuildStreamPayload(88, 0, streamData);

    std::vector<eular::utp::FrameMetaInfo> metas(1);
    metas[0].offset = UTP_HEADER_SIZE;
    metas[0].length = static_cast<uint16_t>(streamBytes.size());
    metas[0].frame_type = FrameType::kFrameStream;
    metas[0].frame_flags = static_cast<uint8_t>(eular::utp::kFMRetransMustKeep | eular::utp::kFMSplittable);
    metas[0].fmi_u.data = 0;

    PacketOut *lostPkt = BuildLostPacket(clientConn,
                                         streamBytes,
                                         metas,
                                         (1u << static_cast<uint32_t>(FrameType::kFrameStream)),
                                         streamData.size(),
                                         0);

    const utp_packno_t beforePackNo = LargestUnackedPackNo(clientConn->m_sendCtl.get());
    REQUIRE(clientConn->m_sendCtl->retransmitSplitStreamPacket(lostPkt, eular::utp::time::MonotonicUs())
             == 0);

    const size_t newStreamPackets = CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(),
                                                                      beforePackNo,
                                                                      (1u << static_cast<uint32_t>(FrameType::kFrameStream)));
    REQUIRE(newStreamPackets >= 2);
    REQUIRE(SumUnackedStreamBytesAfter(clientConn->m_sendCtl.get(), beforePackNo) == streamData.size());
}

TEST_CASE("Retrans repack: mixed frames keep mandatory control frame and drop transient ACK", "[Retrans][Repack]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "")  == 0);
    REQUIRE(client.bind("127.0.0.1", 0, "")  == 0);

    ConnectionImpl::SP clientConn;
    ConnectionImpl::SP serverConn;
    REQUIRE(ConnectPair(loop, server, client, clientConn, serverConn));

    clientConn->m_handshakeDonePending = false;
    clientConn->m_handshakeDoneSent = false;

    const std::vector<uint8_t> ackBytes = {static_cast<uint8_t>(FrameType::kFrameAck), 0x00, 0x00, 0x00};
    const std::string streamData = "mixed-control-stream";
    std::vector<uint8_t> streamBytes = BuildStreamPayload(99, 0, streamData);

    FrameHandshakeDone hsDone;
    hsDone.ack_handshake_pn = 123;
    Status st;
    std::vector<uint8_t> hsBytes(static_cast<size_t>(FRAME_HANDSHAKE_DONE_SIZE), 0);
    REQUIRE(hsDone.encode(hsBytes.data(), hsBytes.size(), st) == FRAME_HANDSHAKE_DONE_SIZE);

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), ackBytes.begin(), ackBytes.end());
    payload.insert(payload.end(), streamBytes.begin(), streamBytes.end());
    payload.insert(payload.end(), hsBytes.begin(), hsBytes.end());

    std::vector<eular::utp::FrameMetaInfo> metas(3);
    metas[0].offset = UTP_HEADER_SIZE;
    metas[0].length = static_cast<uint16_t>(ackBytes.size());
    metas[0].frame_type = FrameType::kFrameAck;
    metas[0].frame_flags = static_cast<uint8_t>(eular::utp::kFMTransientOnRetrans | eular::utp::kFMDroppableOnMtu);
    metas[0].fmi_u.data = 0;
    metas[1].offset = static_cast<uint16_t>(UTP_HEADER_SIZE + ackBytes.size());
    metas[1].length = static_cast<uint16_t>(streamBytes.size());
    metas[1].frame_type = FrameType::kFrameStream;
    metas[1].frame_flags = static_cast<uint8_t>(eular::utp::kFMRetransMustKeep | eular::utp::kFMSplittable);
    metas[1].fmi_u.data = 0;
    metas[2].offset = static_cast<uint16_t>(UTP_HEADER_SIZE + ackBytes.size() + streamBytes.size());
    metas[2].length = static_cast<uint16_t>(hsBytes.size());
    metas[2].frame_type = FrameType::kFrameHandshakeDone;
    metas[2].frame_flags = static_cast<uint8_t>(eular::utp::kFMRetransMustKeep);
    metas[2].fmi_u.data = 0;

        PacketOut *lostPkt = BuildLostPacket(clientConn,
                                         payload,
                                         metas,
                                         (1u << static_cast<uint32_t>(FrameType::kFrameAck))
                                       | (1u << static_cast<uint32_t>(FrameType::kFrameStream))
                                       | (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone))
                                       | (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDelay)),
                                         streamData.size(),
                                         static_cast<uint16_t>(ackBytes.size()));

    const utp_packno_t beforePackNo = LargestUnackedPackNo(clientConn->m_sendCtl.get());
    REQUIRE(clientConn->m_sendCtl->retransmitSplitStreamPacket(lostPkt, eular::utp::time::MonotonicUs())
             == 0);

    REQUIRE(CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(),
                                             beforePackNo,
                                             (1u << static_cast<uint32_t>(FrameType::kFrameAck))) == 0);
    REQUIRE(CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(),
                                             beforePackNo,
                                             (1u << static_cast<uint32_t>(FrameType::kFrameHandshakeDone))) >= 1);
    REQUIRE(CountUnackedPacketsAfterWithBits(clientConn->m_sendCtl.get(),
                                             beforePackNo,
                                             (1u << static_cast<uint32_t>(FrameType::kFrameStream))) >= 1);
}
