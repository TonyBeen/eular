/*************************************************************************
    > File Name: test_fault_injection_new.cc
    > Author: copilot
    > Brief: Fault injection tests for packet_out, mm, udp, crypto
    > Created Time: 2026-05-03
 ************************************************************************/

#include <catch2/catch.hpp>

#include <vector>

#include "utp/errno.h"
#include "utp/platform.h"
#include "utp/config.h"

#include <event/loop.h>
#define private public
#include "context/context_impl.h"
#include "context/connection_impl.h"
#include "socket/udp.h"
#include "proto/packet_out.h"
#include "crypto/aes_gcm_context.h"
#undef private

#include "util/fiu_local.h"

using eular::utp::Config;
using eular::utp::ContextImpl;
using eular::utp::ConnectionImpl;
using eular::utp::PacketOut;
using eular::utp::PacketOutFlags;
using eular::utp::AesGcmContext;
using eular::utp::UdpSocket;

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: mem/packet_out_attempt/alloc
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Fault injection: packet_out_attempt allocation failure", "[FaultInjection][PacketOut]")
{
#if defined(OS_LINUX) && defined(UTP_ENABLE_FAULT_INJECTION)
    REQUIRE(fiu_init(0) == 0);

    ev::EventLoop loop;
    Config cfg;
    ContextImpl ctx(loop.loop(), &cfg);

    PacketOut *pkt = ctx.m_mm.getPacketOut(256);
    REQUIRE(pkt != nullptr);

    // With injection: addSendAttempt must fail immediately
    REQUIRE(fiu_enable("mem/packet_out_attempt/alloc", 1, NULL, 0) == 0);
    REQUIRE_FALSE(pkt->addSendAttempt(1, 1000));
    REQUIRE(fiu_disable("mem/packet_out_attempt/alloc") == 0);

    // Without injection: succeeds
    REQUIRE(pkt->addSendAttempt(2, 2000));

    ctx.m_mm.putPacketOut(pkt);
#else
    SUCCEED();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: mem/mm/packet_out_buf
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Fault injection: mm packet_out buffer malloc failure", "[FaultInjection][MM]")
{
#if defined(OS_LINUX) && defined(UTP_ENABLE_FAULT_INJECTION)
    REQUIRE(fiu_init(0) == 0);

    ev::EventLoop loop;
    Config cfg;
    ContextImpl ctx(loop.loop(), &cfg);

    // Pool is empty on a fresh ContextImpl; getPacketOut must malloc.
    REQUIRE(fiu_enable("mem/mm/packet_out_buf", 1, NULL, 0) == 0);
    PacketOut *pkt = ctx.m_mm.getPacketOut(64);
    REQUIRE(pkt == nullptr);
    REQUIRE(fiu_disable("mem/mm/packet_out_buf") == 0);

    // Without injection: succeeds
    pkt = ctx.m_mm.getPacketOut(64);
    REQUIRE(pkt != nullptr);
    ctx.m_mm.putPacketOut(pkt);
#else
    SUCCEED();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: net/udp/sendto
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Fault injection: udp sendto failure", "[FaultInjection][UDP]")
{
#if defined(OS_LINUX) && defined(UTP_ENABLE_FAULT_INJECTION)
    REQUIRE(fiu_init(0) == 0);

    Config cfg;
    UdpSocket sock(cfg);
    REQUIRE(sock.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    eular::utp::Address peer("127.0.0.1", 12345);

    UdpSocket::MsgMetaInfo msg{};
    msg.data = payload;
    msg.len  = sizeof(payload);
    msg.metaInfo.peerAddress = peer;

    std::vector<UdpSocket::MsgMetaInfo> msgVec = {msg};

    // Without injection: send succeeds (returns 1 = sent count)
    int32_t ret = sock.send(msgVec);
    REQUIRE(ret == 1);

    // With injection: sendto/sendmsg faked to return ENOBUFS
    REQUIRE(fiu_enable("net/udp/sendto", 1, NULL, 0) == 0);
    ret = sock.send(msgVec);
    REQUIRE(ret < 1);
    REQUIRE(utp_get_last_error() == UTP_ERR_SOCKET_WRITE);
    REQUIRE(fiu_disable("net/udp/sendto") == 0);

    // After disabling: send works again
    ret = sock.send(msgVec);
    REQUIRE(ret == 1);
#else
    SUCCEED();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: crypto/encrypt_buf/malloc
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Fault injection: encrypt buffer malloc failure", "[FaultInjection][Crypto]")
{
#if defined(OS_LINUX) && defined(UTP_ENABLE_FAULT_INJECTION)
    REQUIRE(fiu_init(0) == 0);

    ev::EventLoop loop;
    Config cfg;
    ContextImpl ctx(loop.loop(), &cfg);

    AesGcmContext::AesKey256 key;
    key.fill(0x42);
    AesGcmContext aes;
    REQUIRE(aes.init(key, 0x01020304));

    // Build a minimal PacketOut with kPoKeepPlaintext so encrypt() uses the
    // malloc path (alloc_size buffer is not large enough for ciphertext).
    PacketOut *pkt = ctx.m_mm.getPacketOut(256);
    REQUIRE(pkt != nullptr);

    constexpr uint16_t kHdrSize  = 20; // UTP_HEADER_SIZE
    constexpr uint16_t kBodySize = 8;
    pkt->data_size = kHdrSize + kBodySize;
    pkt->po_flags  = static_cast<uint16_t>(PacketOutFlags::kPoKeepPlaintext);
    pkt->packno    = 1;

    // Without injection: encrypt succeeds
    REQUIRE(aes.encrypt(pkt) == 0);
    if (pkt->encrypt_data != nullptr && pkt->encrypt_data != pkt->raw_data) {
        std::free(pkt->encrypt_data);
        pkt->encrypt_data = nullptr;
    }
    pkt->po_flags = static_cast<uint16_t>(PacketOutFlags::kPoKeepPlaintext);

    // With injection: malloc fails → encrypt returns non-zero
    REQUIRE(fiu_enable("crypto/encrypt_buf/malloc", 1, NULL, 0) == 0);
    REQUIRE(aes.encrypt(pkt) != 0);
    REQUIRE(utp_get_last_error() == UTP_ERR_NO_MEMORY);
    REQUIRE(fiu_disable("crypto/encrypt_buf/malloc") == 0);

    // After disabling: encrypt works again
    pkt->po_flags = static_cast<uint16_t>(PacketOutFlags::kPoKeepPlaintext);
    REQUIRE(aes.encrypt(pkt) == 0);

    ctx.m_mm.putPacketOut(pkt);
#else
    SUCCEED();
#endif
}
