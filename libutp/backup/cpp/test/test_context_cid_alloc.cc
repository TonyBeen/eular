/*************************************************************************
    > File Name: test_context_cid_alloc.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 19 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#define private public
#include "context/context_impl.h"
#undef private

#include "utp/config.h"
#include "util/time.h"
#include "socket/address.h"

using eular::utp::Config;
using eular::utp::Status;
using eular::utp::ContextImpl;
using eular::utp::Status;
using eular::utp::Address;
using eular::utp::Status;

TEST_CASE("ContextImpl: allocLocalCid avoids established and pending cid sets", "[Context][CID]")
{
    Config cfg;
    ContextImpl ctx(nullptr, &cfg);

    ctx.m_connections.emplace(1001u, eular::utp::ConnectionImpl::SP());

    ContextImpl::PendingIncomingConnection pending;
    pending.localCid = 1002u;
    ctx.m_pendingIncoming.emplace(1002u, pending);

    uint32_t cid = 0;
    REQUIRE(ctx.allocLocalCid(cid));
    REQUIRE(cid != 0);
    REQUIRE(ctx.m_connections.find(cid) == ctx.m_connections.end());
    REQUIRE(ctx.m_pendingIncoming.find(cid) == ctx.m_pendingIncoming.end());
}

TEST_CASE("ContextImpl: passive handshake timeout recycles pending entry", "[Context][Passive]")
{
    Config cfg;
    cfg.handshake_timeout = 1;
    cfg.handshake_max_retries = 0;
    ContextImpl ctx(nullptr, &cfg);

    int32_t connectErrorCalls = 0;
    ctx.setOnConnectError([&connectErrorCalls](int32_t, const std::string &, eular::utp::Context::ConnectAttemptInfo) {
        ++connectErrorCalls;
    });

    ContextImpl::PendingIncomingConnection pending;
    pending.localCid = 3001u;
    pending.peerCid = 401u;
    pending.peerAddress = Address("127.0.0.1", 9999);
    pending.peerIp = "127.0.0.1";
    pending.handshakeSent = true;
    pending.acceptStartUs = eular::utp::time::MonotonicUs() - 10 * 1000; // bookkeeping only
    pending.lastHandshakeSentUs = eular::utp::time::MonotonicUs() - 2 * 1000;

    ctx.m_pendingIncoming.emplace(pending.localCid, pending);
    ContextImpl::PeerIndexKey peerKey;
    peerKey.peerAddress = pending.peerAddress;
    peerKey.peerCid = pending.peerCid;
    ctx.m_pendingIncomingPeerIndex.emplace(peerKey, pending.localCid);
    ctx.m_waitHandshakeDone.insert(pending.localCid);
    ctx.m_pendingIncomingQueue.push_back(pending.localCid);

    ctx.processPendingHandshakeTimeouts();

    REQUIRE(ctx.m_pendingIncoming.find(pending.localCid) == ctx.m_pendingIncoming.end());
    REQUIRE(ctx.m_waitHandshakeDone.find(pending.localCid) == ctx.m_waitHandshakeDone.end());
    REQUIRE(ctx.m_pendingIncomingQueue.empty());
    REQUIRE(connectErrorCalls == 1);
}

TEST_CASE("ContextImpl: unaccepted pending entry expires and releases peer quota", "[Context][Passive][Limit]")
{
    Config cfg;
    cfg.pending_unaccepted_timeout_ms = 1;
    ContextImpl ctx(nullptr, &cfg);

    ContextImpl::PendingIncomingConnection pending;
    pending.localCid = 3101u;
    pending.peerCid = 4101u;
    pending.peerAddress = Address("127.0.0.1", 9998);
    pending.peerIp = "127.0.0.1";
    pending.initialReceivedUs = eular::utp::time::MonotonicUs() - 2000;

    ctx.m_pendingIncoming.emplace(pending.localCid, pending);
    ctx.m_pendingIncomingQueue.push_back(pending.localCid);
    ctx.notePendingIncomingAdded(pending.peerIp);
    REQUIRE(ctx.canCreatePendingIncoming(pending.peerIp));

    ctx.processPendingHandshakeTimeouts();
    REQUIRE(ctx.m_pendingIncoming.empty());
    REQUIRE(ctx.m_pendingIncomingQueue.empty());
    REQUIRE(ctx.m_pendingIncomingByPeerIp.empty());
}

TEST_CASE("ContextImpl: pending admission enforces global and per-IP quotas", "[Context][Passive][Limit]")
{
    Config cfg;
    cfg.pending_incoming_limit = 2;
    cfg.pending_incoming_per_ip_limit = 1;
    ContextImpl ctx(nullptr, &cfg);

    ContextImpl::PendingIncomingConnection first;
    first.localCid = 3201u;
    first.peerIp = "10.0.0.1";
    ctx.m_pendingIncoming.emplace(first.localCid, first);
    ctx.notePendingIncomingAdded(first.peerIp);

    REQUIRE_FALSE(ctx.canCreatePendingIncoming("10.0.0.1"));
    REQUIRE(ctx.canCreatePendingIncoming("10.0.0.2"));

    ContextImpl::PendingIncomingConnection second;
    second.localCid = 3202u;
    second.peerIp = "10.0.0.2";
    ctx.m_pendingIncoming.emplace(second.localCid, second);
    ctx.notePendingIncomingAdded(second.peerIp);
    REQUIRE_FALSE(ctx.canCreatePendingIncoming("10.0.0.3"));
}
