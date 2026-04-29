/*************************************************************************
    > File Name: test_passive_conn_extra.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 29 Apr 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <event/loop.h>
#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#undef private

#include "proto/proto.h"
#include "proto/frame/handshake_done.h"
#include "util/time.h"

using eular::utp::Address;
using eular::utp::Config;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::UdpSocket;
using eular::utp::PacketIn;
using eular::utp::FrameHandshakeDone;
using eular::Serialize;

namespace {

UdpSocket::MsgMetaInfo BuildInitialPacket(uint32_t scid, uint32_t dcid, uint64_t pn, std::vector<uint8_t> &buffer)
{
    buffer.resize(eular::utp::UTP_HEADER_SIZE + 10); // Small payload
    uint8_t *offset = buffer.data();
    size_t left = buffer.size();

    offset = Serialize::SerializeTo(offset, left, scid);
    offset = Serialize::SerializeTo(offset, left, dcid);
    offset = Serialize::SerializeTo(offset, left, pn);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(10));
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(eular::utp::UTP_TYPE_INITIAL));
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));

    UdpSocket::MsgMetaInfo msg;
    msg.data = buffer.data();
    msg.len = buffer.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 12345);
    return msg;
}

UdpSocket::MsgMetaInfo BuildHandshakeDonePacket(uint32_t scid, uint32_t dcid, uint64_t pn, utp_packno_t ackHandshakePn, std::vector<uint8_t> &buffer)
{
    FrameHandshakeDone done;
    done.ack_handshake_pn = ackHandshakePn;
    
    buffer.resize(eular::utp::UTP_HEADER_SIZE + done.frameSize());
    uint8_t *offset = buffer.data();
    size_t left = buffer.size();

    offset = Serialize::SerializeTo(offset, left, scid);
    offset = Serialize::SerializeTo(offset, left, dcid);
    offset = Serialize::SerializeTo(offset, left, pn);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(done.frameSize()));
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(eular::utp::UTP_TYPE_CTRL));
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    
    done.encode(offset, left);

    UdpSocket::MsgMetaInfo msg;
    msg.data = buffer.data();
    msg.len = buffer.size();
    msg.metaInfo.peerAddress = Address("127.0.0.1", 12345);
    return msg;
}

} // namespace

TEST_CASE("Passive Connection: handle duplicate Initial", "[Passive][Handshake]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    bool notified = false;
    ctx.setOnNewConnection([&](const Context::NewConnectionInfo &) {
        notified = true;
        return true;
    });

    std::vector<uint8_t> buf1;
    UdpSocket::MsgMetaInfo msg1 = BuildInitialPacket(100, 0, 1, buf1);

    ctx.onReadPacket(msg1);
    REQUIRE(notified);
    REQUIRE(ctx.m_pendingIncoming.size() == 1);
    REQUIRE(ctx.m_pendingIncomingQueue.size() == 1);

    // Send duplicate Initial
    notified = false;
    ctx.onReadPacket(msg1);
    REQUIRE_FALSE(notified); // Should not notify again if already pending
    REQUIRE(ctx.m_pendingIncoming.size() == 1);
}

TEST_CASE("Passive Connection: HandshakeDone convergence", "[Passive][Handshake]")
{
    Config cfg;
    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    ctx.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    std::vector<uint8_t> buf1;
    UdpSocket::MsgMetaInfo msg1 = BuildInitialPacket(100, 0, 1, buf1);
    ctx.onReadPacket(msg1);

    uint32_t localCid = ctx.m_pendingIncomingQueue.front();
    REQUIRE(ctx.accept() == UTP_ERR_OK);
    
    auto &pending = ctx.m_pendingIncoming[localCid];
    REQUIRE(pending.handshakeSent);
    utp_packno_t handshakePn = pending.lastHandshakePacketNo;

    // Receive HandshakeDone
    std::vector<uint8_t> buf2;
    UdpSocket::MsgMetaInfo msg2 = BuildHandshakeDonePacket(100, localCid, 2, handshakePn, buf2);
    
    bool connectedCalled = false;
    ctx.setOnConnected([&](eular::utp::Connection::Ptr) {
        connectedCalled = true;
    });

    ctx.onReadPacket(msg2);
    REQUIRE(connectedCalled);
    REQUIRE(ctx.m_pendingIncoming.empty());
    REQUIRE(ctx.m_connections.count(localCid));
}
