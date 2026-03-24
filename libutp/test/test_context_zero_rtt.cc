/*************************************************************************
    > File Name: test_context_zero_rtt.cc
    > Author: eular
    > Brief:
    > Created Time: Sat 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <event/loop.h>

#define private public
#include "context/context_impl.h"
#undef private

#include "crypto/token.h"
#include "util/time.h"

using eular::utp::Address;
using eular::utp::Config;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::TokenAuth;
using eular::utp::TokenMeta;
using eular::utp::TokenType;

namespace {

TokenMeta BuildZeroRttMeta(const Address &address, uint32_t timestamp)
{
    TokenMeta meta;
    meta.token_type = static_cast<uint8_t>(TokenType::kZeroRttResumption);
    meta.timestamp = timestamp;
    meta.cid = 12345;
    meta.encryption_mode = static_cast<uint8_t>(Context::kEncryptionAesGcm256);
    meta.version = 1;
    meta.secret = 7;
    meta.family = static_cast<uint16_t>(address.family());

    if (address.isIPv4()) {
        sockaddr_in addr4{};
        REQUIRE(address.toSockAddrIn(addr4));
        meta.host_v4 = addr4.sin_addr;
    } else {
        sockaddr_in6 addr6{};
        REQUIRE(address.toSockAddrIn6(addr6));
        meta.host_v6 = addr6.sin6_addr;
    }

    return meta;
}

std::vector<uint8_t> BuildToken(TokenAuth *auth, const TokenMeta &meta)
{
    REQUIRE(auth != nullptr);

    TokenAuth::TokenBuf tokenBuf{};
    REQUIRE(auth->seal(meta, tokenBuf));
    return std::vector<uint8_t>(tokenBuf.begin(), tokenBuf.end());
}

} // namespace

TEST_CASE("ContextImpl: validateZeroRttTicket accepts valid bound token", "[Context][0RTT]")
{
    Config cfg;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    Address peer("127.0.0.1", 9000);
    const uint32_t nowSec = static_cast<uint32_t>(eular::utp::time::RealtimeMs() / 1000);
    TokenMeta meta = BuildZeroRttMeta(peer, nowSec);
    std::vector<uint8_t> token = BuildToken(ctx.tokenAuth(), meta);

    uint32_t ticketCid = 0;
    Context::EncryptionMode mode = Context::kEncryptionNone;
    REQUIRE(ctx.validateZeroRttTicket(peer, token, 300, ticketCid, mode));
    REQUIRE(ticketCid == meta.cid);
    REQUIRE(mode == Context::kEncryptionAesGcm256);
}

TEST_CASE("ContextImpl: validateZeroRttTicket rejects peer mismatch", "[Context][0RTT]")
{
    Config cfg;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    Address tokenPeer("127.0.0.1", 9000);
    Address packetPeer("127.0.0.2", 9000);
    const uint32_t nowSec = static_cast<uint32_t>(eular::utp::time::RealtimeMs() / 1000);
    TokenMeta meta = BuildZeroRttMeta(tokenPeer, nowSec);
    std::vector<uint8_t> token = BuildToken(ctx.tokenAuth(), meta);

    uint32_t ticketCid = 0;
    Context::EncryptionMode mode = Context::kEncryptionNone;
    REQUIRE_FALSE(ctx.validateZeroRttTicket(packetPeer, token, 300, ticketCid, mode));
}

TEST_CASE("ContextImpl: validateZeroRttTicket rejects expired token", "[Context][0RTT]")
{
    Config cfg;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    Address peer("127.0.0.1", 9000);
    const uint32_t nowSec = static_cast<uint32_t>(eular::utp::time::RealtimeMs() / 1000);
    TokenMeta meta = BuildZeroRttMeta(peer, nowSec - 20);
    std::vector<uint8_t> token = BuildToken(ctx.tokenAuth(), meta);

    uint32_t ticketCid = 0;
    Context::EncryptionMode mode = Context::kEncryptionNone;
    REQUIRE_FALSE(ctx.validateZeroRttTicket(peer, token, 10, ticketCid, mode));
}

TEST_CASE("ContextImpl: rememberZeroRttNonce rejects duplicates within replay window", "[Context][0RTT]")
{
    Config cfg;
    cfg.zero_rtt_replay_window = 1;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);

    REQUIRE(ctx.rememberZeroRttNonce(777, 1001, 1000));
    REQUIRE_FALSE(ctx.rememberZeroRttNonce(777, 1001, 1001));

    ctx.purgeZeroRttReplayCache(2501);
    REQUIRE(ctx.rememberZeroRttNonce(777, 1001, 2502));
}
