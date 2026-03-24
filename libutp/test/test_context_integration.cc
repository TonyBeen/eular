/*************************************************************************
    > File Name: test_context_integration.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 19 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <chrono>
#include <cstdint>
#include <array>
#include <thread>

#include <event2/event.h>
#include <event/loop.h>

#define private public
#include "context/context_impl.h"
#undef private

#include "crypto/token.h"
#include "utp/errno.h"
#include "util/random.hpp"
#include "util/time.h"

using eular::utp::Address;
using eular::utp::Config;
using eular::utp::Connection;
using eular::utp::ConnectionImpl;
using eular::utp::Context;
using eular::utp::ContextImpl;
using eular::utp::TokenAuth;
using eular::utp::TokenMeta;
using eular::utp::TokenType;

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

ConnectionImpl::SP FindConnectionByRemote(const ContextImpl &ctx, uint16_t port)
{
    for (const auto &entry : ctx.m_connections) {
        if (entry.second && entry.second->connectInfo().port == port) {
            return entry.second;
        }
    }
    return ConnectionImpl::SP();
}

bool HasConnectedConnection(const ContextImpl &ctx)
{
    for (const auto &entry : ctx.m_connections) {
        if (entry.second && entry.second->state() == ConnectionImpl::kStateConnected) {
            return true;
        }
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

TokenMeta BuildZeroRttMeta(const Address &address,
                           uint32_t timestamp,
                           uint32_t cid,
                           Context::EncryptionMode encryptionMode)
{
    TokenMeta meta;
    meta.token_type = static_cast<uint8_t>(TokenType::kZeroRttResumption);
    meta.timestamp = timestamp;
    meta.cid = cid;
    meta.encryption_mode = static_cast<uint8_t>(encryptionMode);
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

std::vector<uint8_t> BuildZeroRttTicket(ContextImpl &ctx,
                                        const Address &address,
                                        uint32_t cid = 12345,
                                        Context::EncryptionMode encryptionMode = Context::kEncryptionNone)
{
    TokenAuth *auth = ctx.tokenAuth();
    REQUIRE(auth != nullptr);

    const uint32_t nowSec = static_cast<uint32_t>(eular::utp::time::RealtimeMs() / 1000);
    const TokenMeta meta = BuildZeroRttMeta(address, nowSec, cid, encryptionMode);

    TokenAuth::TokenBuf tokenBuf{};
    REQUIRE(auth->seal(meta, tokenBuf));
    return std::vector<uint8_t>(tokenBuf.begin(), tokenBuf.end());
}

} // namespace

TEST_CASE("Context integration: OnNewConnection allow completes passive handshake", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool serverNotified = false;
    bool serverConnected = false;
    bool clientConnected = false;
    bool accepted = false;
    int32_t acceptStatus = UTP_ERR_WOULD_BLOCK;

    server.setOnNewConnection([&serverNotified](const Context::NewConnectionInfo &) {
        serverNotified = true;
        return true;
    });
    server.setOnConnected([&serverConnected](Connection::Ptr) {
        serverConnected = true;
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() { return HasConnectedConnection(server) && HasConnectedConnection(client); },
        [&]() {
            if (!accepted && !server.m_pendingIncomingQueue.empty()) {
                acceptStatus = server.accept();
                if (acceptStatus == UTP_ERR_OK) {
                    accepted = true;
                }
            }
        });

    REQUIRE(ok);
    REQUIRE(serverNotified);
    REQUIRE(serverConnected);
    REQUIRE(clientConnected);
    REQUIRE(accepted);
    REQUIRE(acceptStatus == UTP_ERR_OK);
}

TEST_CASE("Context integration: OnNewConnection may accept immediately", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool serverNotified = false;
    bool serverConnected = false;
    bool clientConnected = false;
    int32_t acceptStatus = UTP_ERR_WOULD_BLOCK;

    server.setOnNewConnection([&](const Context::NewConnectionInfo &) {
        serverNotified = true;
        acceptStatus = server.accept();
        return acceptStatus == UTP_ERR_OK;
    });
    server.setOnConnected([&serverConnected](Connection::Ptr) {
        serverConnected = true;
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() { return HasConnectedConnection(server) && HasConnectedConnection(client); },
        nullptr);

    REQUIRE(ok);
    REQUIRE(serverNotified);
    REQUIRE(serverConnected);
    REQUIRE(clientConnected);
    REQUIRE(acceptStatus == UTP_ERR_OK);
    REQUIRE(server.m_pendingIncomingQueue.empty());
}

TEST_CASE("Context integration: OnNewConnection reject returns connection close to client", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool serverNotified = false;
    bool serverConnected = false;
    bool clientConnected = false;

    server.setOnNewConnection([&serverNotified](const Context::NewConnectionInfo &) {
        serverNotified = true;
        return false;
    });
    server.setOnConnected([&serverConnected](Connection::Ptr) {
        serverConnected = true;
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            if (!serverNotified || client.m_connections.empty()) {
                return false;
            }
            return client.m_connections.begin()->second->state() == ConnectionImpl::kStateCloseReceived;
        },
        nullptr);

    REQUIRE(ok);
    REQUIRE(serverNotified);
    REQUIRE_FALSE(serverConnected);
    REQUIRE_FALSE(clientConnected);
    REQUIRE(server.m_pendingIncoming.empty());
}

TEST_CASE("Context integration: mixed active and passive handshakes share one Context and avoid pending cid", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl hub(loop.loop(), &cfg);
    ContextImpl incomingClient(loop.loop(), &cfg);
    ContextImpl remoteServer(loop.loop(), &cfg);

    REQUIRE(hub.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(incomingClient.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(remoteServer.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool hubIncomingNotified = false;
    bool hubAcceptedIncoming = false;
    bool remoteAccepted = false;
    bool outboundStarted = false;
    uint32_t forcedCollisionCid = 0;
    uint32_t outboundCid = 0;
    int32_t hubConnectedCount = 0;
    int32_t incomingClientConnectedCount = 0;
    int32_t remoteServerConnectedCount = 0;

    hub.setOnNewConnection([&hubIncomingNotified](const Context::NewConnectionInfo &) {
        hubIncomingNotified = true;
        return true;
    });
    hub.setOnConnected([&hubConnectedCount](Connection::Ptr) {
        ++hubConnectedCount;
    });
    incomingClient.setOnConnected([&incomingClientConnectedCount](Connection::Ptr) {
        ++incomingClientConnectedCount;
    });
    remoteServer.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });
    remoteServer.setOnConnected([&remoteServerConnectedCount](Connection::Ptr) {
        ++remoteServerConnectedCount;
    });

    Context::ConnectInfo incomingInfo;
    incomingInfo.ip = "127.0.0.1";
    incomingInfo.port = BoundPort(hub);
    incomingInfo.timeout = 200;
    REQUIRE(incomingClient.connect(incomingInfo) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            return hubConnectedCount >= 2 && incomingClientConnectedCount >= 1 && remoteServerConnectedCount >= 1;
        },
        [&]() {
            if (hubIncomingNotified && !outboundStarted) {
                auto genCopy = eular::utp::rng();
                std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
                forcedCollisionCid = dist(genCopy);

                ContextImpl::PendingIncomingConnection syntheticPending;
                syntheticPending.localCid = forcedCollisionCid;
                hub.m_pendingIncoming.emplace(forcedCollisionCid, syntheticPending);

                Context::ConnectInfo outboundInfo;
                outboundInfo.ip = "127.0.0.1";
                outboundInfo.port = BoundPort(remoteServer);
                outboundInfo.timeout = 200;
                REQUIRE(hub.connect(outboundInfo) == UTP_ERR_OK);

                ConnectionImpl::SP outbound = FindConnectionByRemote(hub, outboundInfo.port);
                REQUIRE(outbound != nullptr);
                outboundCid = outbound->cid();
                REQUIRE(outboundCid != forcedCollisionCid);
                outboundStarted = true;
            }

            if (hubIncomingNotified && !hubAcceptedIncoming) {
                const int32_t status = hub.accept();
                if (status == UTP_ERR_OK) {
                    hubAcceptedIncoming = true;
                }
            }

            if (!remoteAccepted && !remoteServer.m_pendingIncomingQueue.empty()) {
                REQUIRE(remoteServer.accept() == UTP_ERR_OK);
                remoteAccepted = true;
            }
        });

    REQUIRE(ok);
    REQUIRE(hubAcceptedIncoming);
    REQUIRE(remoteAccepted);
    REQUIRE(outboundStarted);
    REQUIRE(outboundCid != 0);
}

TEST_CASE("Context integration: 0-RTT accepted early data establishes directly", "[Context][Integration][0RTT]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool serverConnected = false;
    bool clientConnected = false;
    bool serverStreamCreated = false;
    bool connectedBeforeStream = false;
    Context::EncryptionMode observedEncryption = Context::kEncryptionNone;
    uint32_t earlyStreamId = UINT32_MAX;
    const std::string earlyPayload = "hello-0rtt";
    const std::vector<uint8_t> ticket = BuildZeroRttTicket(server,
                                                           Address("127.0.0.1", BoundPort(client)),
                                                           12345,
                                                           Context::kEncryptionAesGcm256);

    server.setOnNewConnection([&](const Context::NewConnectionInfo &newInfo) {
        observedEncryption = newInfo.encrypted;
        return true;
    });
    server.setOnConnected([&](Connection::Ptr conn) {
        serverConnected = true;
        conn->registerStreamCreated([&](eular::utp::Stream *stream) {
            REQUIRE(stream != nullptr);
            connectedBeforeStream = serverConnected;
            serverStreamCreated = true;
            earlyStreamId = stream->id();
        });
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });

    Context::Connect0RttInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 300;
    info.session_ticket = ticket;
    info.early_data.assign(earlyPayload.begin(), earlyPayload.end());
    REQUIRE(client.connect0Rtt(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr
                && serverConnected
                && clientConnected
                && serverStreamCreated;
        },
        nullptr,
        400,
        1);

    REQUIRE(ok);
    REQUIRE(connectedBeforeStream);
    REQUIRE(server.m_pendingIncoming.empty());
    REQUIRE(server.m_pendingIncomingQueue.empty());

    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    REQUIRE(serverConn != nullptr);
    REQUIRE(observedEncryption == Context::kEncryptionAesGcm256);
    REQUIRE(serverConn->connectInfo().encrypted == Context::kEncryptionAesGcm256);
    REQUIRE(earlyStreamId == 0);

    auto serverStreamIt = serverConn->m_streams.find(earlyStreamId);
    REQUIRE(serverStreamIt != serverConn->m_streams.end());
    REQUIRE(serverStreamIt->second != nullptr);

    std::array<char, 64> readBuf{};
    const int32_t nread = serverStreamIt->second->read(readBuf.data(), readBuf.size());
    REQUIRE(nread == static_cast<int32_t>(earlyPayload.size()));
    REQUIRE(std::string(readBuf.data(), static_cast<size_t>(nread)) == earlyPayload);

    const Context::Statistic stat = server.statistic();
    REQUIRE(stat.zero_rtt_offered >= 1);
    REQUIRE(stat.zero_rtt_accepted >= 1);
    REQUIRE(stat.zero_rtt_rejected == 0);
}

TEST_CASE("Context integration: 0-RTT rejected ticket closes client without delivering early stream", "[Context][Integration][0RTT]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool serverNotified = false;
    bool clientConnected = false;
    int32_t zeroRttAcceptedEvents = 0;
    int32_t zeroRttRejectedEvents = 0;
    std::string lastZeroRttReason;
    const std::string earlyPayload = "drop-me";

    server.setOnNewConnection([&serverNotified](const Context::NewConnectionInfo &) {
        serverNotified = true;
        return true;
    });
    server.setOnZeroRttDecision([&](const Context::ZeroRttDecisionInfo &info) {
        if (info.accepted) {
            ++zeroRttAcceptedEvents;
        } else {
            ++zeroRttRejectedEvents;
            lastZeroRttReason = info.reason;
        }
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });

    Context::Connect0RttInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 300;
    info.session_ticket.assign(16, 0x5a);
    info.early_data.assign(earlyPayload.begin(), earlyPayload.end());
    REQUIRE(client.connect0Rtt(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            if (client.m_connections.empty()) {
                return false;
            }
            return client.m_connections.begin()->second->state() == ConnectionImpl::kStateCloseReceived;
        },
        nullptr,
        400,
        1);

    REQUIRE(ok);
    REQUIRE_FALSE(serverNotified);
    REQUIRE_FALSE(clientConnected);
    REQUIRE(server.m_connections.empty());
    REQUIRE(server.m_pendingIncoming.empty());
    REQUIRE(server.m_pendingIncomingQueue.empty());

    const Context::Statistic stat = server.statistic();
    REQUIRE(stat.zero_rtt_offered >= 1);
    REQUIRE(stat.zero_rtt_rejected >= 1);
    REQUIRE(stat.zero_rtt_accepted == 0);
    REQUIRE(zeroRttAcceptedEvents == 0);
    REQUIRE(zeroRttRejectedEvents >= 1);
    REQUIRE(lastZeroRttReason == "invalid_ticket");
}
