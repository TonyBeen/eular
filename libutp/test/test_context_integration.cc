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
#include <cstring>
#include <thread>

#include <event2/event.h>
#include <event/loop.h>

#include <utils/serialize.hpp>

#define private public
#include "context/context_impl.h"
#undef private

#include "crypto/token.h"
#include "proto/proto.h"
#include "proto/frame.h"
#include "proto/frame/stream.h"
#include "proto/frame/handshake_done.h"
#include "proto/packet_in.h"
#include "proto/frame/path.h"
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
using eular::utp::UdpSocket;
using eular::utp::FramePathChallenge;
using eular::utp::FramePathResponse;
using eular::utp::FrameStream;
using eular::utp::FrameHandshakeDone;
using eular::utp::PacketIn;
using eular::utp::NetworkPath;

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

std::vector<uint8_t> BuildRawPacket(uint32_t scid,
                                    uint32_t dcid,
                                    uint64_t pn,
                                    uint8_t packetType,
                                    const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> packet(static_cast<size_t>(UTP_HEADER_SIZE) + payload.size(), 0);
    uint8_t *offset = packet.data();
    size_t left = packet.size();
    const uint16_t payloadLen = static_cast<uint16_t>(payload.size());
    const uint8_t reserve = 0;

    offset = eular::Serialize::SerializeTo(offset, left, scid);
    offset = eular::Serialize::SerializeTo(offset, left, dcid);
    offset = eular::Serialize::SerializeTo(offset, left, pn);
    offset = eular::Serialize::SerializeTo(offset, left, payloadLen);
    offset = eular::Serialize::SerializeTo(offset, left, packetType);
    offset = eular::Serialize::SerializeTo(offset, left, reserve);
    REQUIRE(offset != nullptr);
    REQUIRE(left == payload.size());

    if (!payload.empty()) {
        std::memcpy(offset, payload.data(), payload.size());
    }
    return packet;
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

TEST_CASE("Context integration: connect retries after first reject when retries > 0", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    int32_t incomingAttempts = 0;
    bool serverConnected = false;
    bool clientConnected = false;
    int32_t clientConnectErrors = 0;
    bool accepted = false;

    server.setOnNewConnection([&incomingAttempts](const Context::NewConnectionInfo &) {
        ++incomingAttempts;
        return incomingAttempts > 1;
    });
    server.setOnConnected([&serverConnected](Connection::Ptr) {
        serverConnected = true;
    });
    client.setOnConnected([&clientConnected](Connection::Ptr) {
        clientConnected = true;
    });
    client.setOnConnectError([&clientConnectErrors](int32_t, const std::string &, Context::ConnectAttemptInfo) {
        ++clientConnectErrors;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    info.retries = 1;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() { return HasConnectedConnection(server) && HasConnectedConnection(client); },
        [&]() {
            if (!accepted && !server.m_pendingIncomingQueue.empty()) {
                accepted = (server.accept() == UTP_ERR_OK);
            }
        },
        5000,
        1);

    REQUIRE(ok);
    REQUIRE(incomingAttempts >= 2);
    REQUIRE(serverConnected);
    REQUIRE(clientConnected);
    REQUIRE(accepted);
    REQUIRE(clientConnectErrors == 0);
}

TEST_CASE("Context integration: remote ConnectionClose converges to OnConnectionClosed callback", "[Context][Integration]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    int32_t clientClosedCallbacks = 0;
    client.setOnConnectionClosed([&](Connection::Ptr) {
        ++clientClosedCallbacks;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() { return HasConnectedConnection(server) && HasConnectedConnection(client); },
        [&]() {
            if (!server.m_pendingIncomingQueue.empty()) {
                (void)server.accept();
            }
        },
        300,
        1));

    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(client));
    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    REQUIRE(serverConn != nullptr);
    REQUIRE(clientConn != nullptr);

    serverConn->close();

    REQUIRE(PumpUntil(
        loop,
        [&]() { return clientConn->state() == ConnectionImpl::kStateCloseReceived; },
        nullptr,
        300,
        1));

    clientConn->onCloseDrainTimeout();
    client.handleConnectionState(clientConn.get());

    REQUIRE(clientClosedCallbacks == 1);
    REQUIRE(client.m_connections.empty());
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
        conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
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
    REQUIRE(stat.zero_rtt_invalid_ticket_rejected >= 1);
    REQUIRE(stat.zero_rtt_accepted == 0);
    REQUIRE(zeroRttAcceptedEvents == 0);
    REQUIRE(zeroRttRejectedEvents >= 1);
    REQUIRE(lastZeroRttReason == "invalid_ticket");
}

TEST_CASE("Context integration: SessionToken callback and export work after handshake", "[Context][Integration][SessionToken]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool accepted = false;
    bool tokenReady = false;
    Connection::Ptr clientConn;

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    client.setOnConnected([&](Connection::Ptr conn) {
        clientConn = conn;
        conn->setOnSessionTokenReady([&]() {
            tokenReady = true;
        });
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 300;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            return accepted && tokenReady && clientConn != nullptr;
        },
        [&]() {
            if (!accepted && !server.m_pendingIncomingQueue.empty()) {
                accepted = (server.accept() == UTP_ERR_OK);
            }
        },
        500,
        1);

    REQUIRE(ok);
    REQUIRE(clientConn != nullptr);

    std::vector<uint8_t> token;
    REQUIRE(clientConn->exportSessionToken(token) == UTP_ERR_OK);
    REQUIRE(token.size() == eular::utp::TOKEN_SIZE);

    std::string state;
    REQUIRE(clientConn->exportSessionResumptionState(state) == UTP_ERR_OK);
    REQUIRE_FALSE(state.empty());
}

TEST_CASE("Context integration: connect0RttWithState succeeds after phase1 close", "[Context][Integration][0RTT][ResumptionState]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    cfg.zero_rtt_token_max_lifetime = 600;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);
    ContextImpl resumer(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(resumer.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool phase1Accepted = false;
    bool phase1Connected = false;
    bool tokenReady = false;
    Connection::Ptr phase1ClientConn;

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });
    client.setOnConnected([&](Connection::Ptr conn) {
        if (!phase1Connected) {
            phase1Connected = true;
            phase1ClientConn = conn;
            conn->setOnSessionTokenReady([&]() {
                tokenReady = true;
            });
        }
    });

    Context::ConnectInfo phase1;
    phase1.ip = "127.0.0.1";
    phase1.port = BoundPort(server);
    phase1.timeout = 300;
    phase1.encrypted = Context::kEncryptionAesGcm256;
    REQUIRE(client.connect(phase1) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return phase1Accepted && phase1Connected && tokenReady && phase1ClientConn != nullptr;
        },
        [&]() {
            if (!phase1Accepted && !server.m_pendingIncomingQueue.empty()) {
                phase1Accepted = (server.accept() == UTP_ERR_OK);
            }
        },
        600,
        1));

    std::string state;
    REQUIRE(phase1ClientConn->exportSessionResumptionState(state) == UTP_ERR_OK);
    REQUIRE_FALSE(state.empty());

    phase1ClientConn->close();
    for (auto &entry : server.m_connections) {
        if (entry.second != nullptr) {
            entry.second->close();
        }
    }
    // Give close notifications a chance to flush, but do not require hard-empty maps
    // because closed connections may linger briefly before deferred cleanup.
    PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) == nullptr
                && FindConnectedByRemote(client, BoundPort(server)) == nullptr;
        },
        nullptr,
        200,
        1);

    bool phase2ServerConnected = false;
    bool phase2ClientConnected = false;
    bool phase2ServerStreamCreated = false;
    const std::string earlyPayload = "state-0rtt";
    uint32_t earlyStreamId = UINT32_MAX;

    server.setOnConnected([&](Connection::Ptr conn) {
        phase2ServerConnected = true;
        conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
            REQUIRE(stream != nullptr);
            phase2ServerStreamCreated = true;
            earlyStreamId = stream->id();
        });
    });
    resumer.setOnConnected([&](Connection::Ptr) {
        phase2ClientConnected = true;
    });

    Context::Connect0RttWithStateInfo phase2;
    phase2.ip = "127.0.0.1";
    phase2.port = BoundPort(server);
    phase2.timeout = 300;
    phase2.early_data.assign(earlyPayload.begin(), earlyPayload.end());
    REQUIRE(resumer.connect0RttWithState(phase2, state) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(resumer)) != nullptr
                && FindConnectedByRemote(resumer, BoundPort(server)) != nullptr
                && phase2ServerConnected
                && phase2ClientConnected
                && phase2ServerStreamCreated;
        },
        nullptr,
        800,
        1));

    ConnectionImpl::SP serverConn = FindConnectedByRemote(server, BoundPort(resumer));
    REQUIRE(serverConn != nullptr);
    REQUIRE(serverConn->connectInfo().encrypted == Context::kEncryptionAesGcm256);
    REQUIRE(earlyStreamId == 0);

    auto streamIt = serverConn->m_streams.find(earlyStreamId);
    REQUIRE(streamIt != serverConn->m_streams.end());
    REQUIRE(streamIt->second != nullptr);

    std::array<char, 64> readBuf{};
    const int32_t nread = streamIt->second->read(readBuf.data(), readBuf.size());
    REQUIRE(nread == static_cast<int32_t>(earlyPayload.size()));
    REQUIRE(std::string(readBuf.data(), static_cast<size_t>(nread)) == earlyPayload);
}

TEST_CASE("Context integration: 0-RTT replay is rejected and counted", "[Context][Integration][0RTT]")
{
    Config cfg;
    cfg.handshake_timeout = 300;
    cfg.zero_rtt_token_max_lifetime = 600;
    cfg.zero_rtt_replay_window = 60;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl clientA(loop.loop(), &cfg);
    ContextImpl clientB(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(clientA.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(clientB.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    int32_t acceptedEvents = 0;
    int32_t replayRejectedEvents = 0;

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });
    server.setOnZeroRttDecision([&](const Context::ZeroRttDecisionInfo &info) {
        if (info.accepted) {
            ++acceptedEvents;
            return;
        }
        if (info.reason == "replay") {
            ++replayRejectedEvents;
        }
    });

    const std::vector<uint8_t> ticket = BuildZeroRttTicket(server,
                                                           Address("127.0.0.1", BoundPort(clientA)));

    Context::Connect0RttInfo first;
    first.ip = "127.0.0.1";
    first.port = BoundPort(server);
    first.timeout = 300;
    first.session_ticket = ticket;
    first.early_data = {'o', 'n', 'e'};
    REQUIRE(clientA.connect0Rtt(first) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() { return acceptedEvents >= 1; },
        nullptr,
        500,
        1));

    Context::Connect0RttInfo second;
    second.ip = "127.0.0.1";
    second.port = BoundPort(server);
    second.timeout = 300;
    second.session_ticket = ticket;
    second.early_data = {'t', 'w', 'o'};
    REQUIRE(clientB.connect0Rtt(second) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() { return replayRejectedEvents >= 1; },
        nullptr,
        500,
        1));

    const Context::Statistic stat = server.statistic();
    REQUIRE(stat.zero_rtt_offered >= 2);
    REQUIRE(stat.zero_rtt_accepted >= 1);
    REQUIRE(stat.zero_rtt_rejected >= 1);
    REQUIRE(stat.zero_rtt_replay_rejected >= 1);
}

TEST_CASE("Context integration: path validation counters track started/succeeded/failed", "[Context][Integration][Path]")
{
    Config cfg;
    cfg.keepalive_timeout = 1;
    cfg.keepalive_probes = 1;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_base = 1400;
    cfg.mtu_max = 1500;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    ConnectionImpl conn(&ctx, &ctx.m_udpSocket, 0x12345678);

    conn.m_state = ConnectionImpl::kStateConnected;
    conn.m_peerAddress = Address("10.0.0.1", 10000);
    conn.m_networkPath.bindPeerAddress(conn.m_peerAddress);

    const Address candidate("10.0.0.1", 10001);

    std::array<uint8_t, UTP_HEADER_SIZE + 1> pingPacket{};
    uint8_t *offset = pingPacket.data();
    size_t left = pingPacket.size();
    const uint32_t scid = 0x87654321;
    const uint32_t dcid = conn.m_localConnectionID;
    const uint64_t pn = 1;
    const uint16_t payloadLen = 1;
    const uint8_t type = UTP_TYPE_CTRL;
    const uint8_t reserve = 0;

    offset = eular::Serialize::SerializeTo(offset, left, scid);
    offset = eular::Serialize::SerializeTo(offset, left, dcid);
    offset = eular::Serialize::SerializeTo(offset, left, pn);
    offset = eular::Serialize::SerializeTo(offset, left, payloadLen);
    offset = eular::Serialize::SerializeTo(offset, left, type);
    offset = eular::Serialize::SerializeTo(offset, left, reserve);
    REQUIRE(offset != nullptr);
    REQUIRE(left == 1);
    offset[0] = static_cast<uint8_t>(eular::utp::kFramePing);

    UdpSocket::MsgMetaInfo msg{};
    msg.data = pingPacket.data();
    msg.len = static_cast<int32_t>(pingPacket.size());
    msg.metaInfo.peerAddress = candidate;

    conn.onUdpPacket(msg);

    Context::Statistic stat = ctx.statistic();
    REQUIRE(stat.path_validation_started >= 1);
    REQUIRE(conn.m_networkPath.needPathValidation());

    REQUIRE(conn.m_networkPath.hasInFlightChallenge());
    FramePathResponse response;
    response.data = conn.m_networkPath.m_pendingChallenge;

    uint8_t responseBuf[FRAME_PATH_FRAME_SIZE] = {0};
    const int32_t responseLen = response.encode(responseBuf, sizeof(responseBuf));
    REQUIRE(responseLen > 0);
    conn.handlePathResponseFrame(responseBuf, static_cast<size_t>(responseLen), candidate);

    stat = ctx.statistic();
    REQUIRE(stat.path_validation_succeeded >= 1);
    REQUIRE(conn.m_peerAddress == candidate);
    REQUIRE(conn.m_mtuDiscovery.pathMtu() == 1400);
    REQUIRE(conn.m_mtuDiscovery.shouldProbe(eular::utp::time::MonotonicMs()));

    const Address activeAfterSuccess = conn.m_peerAddress;
    const Address candidateFail("10.0.0.1", 10002);
    REQUIRE(conn.m_networkPath.detectPeerAddressChange(candidateFail));
    FramePathChallenge failChallenge;
    REQUIRE(conn.m_networkPath.makePathChallenge(failChallenge, 0) == UTP_ERR_OK);

    conn.onPathValidationTimeout();

    stat = ctx.statistic();
    REQUIRE(stat.path_validation_failed >= 1);
    REQUIRE(conn.m_peerAddress == activeAfterSuccess);
    REQUIRE(conn.m_networkPath.state() == NetworkPath::kPathValidated);
}

TEST_CASE("Context integration: handshake promotion preserves first stream callback", "[Context][Integration][Handshake]")
{
    Config cfg;
    cfg.handshake_timeout = 300;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    bool accepted = false;
    bool serverConnected = false;
    bool serverStreamCreated = false;
    bool clientConnected = false;
    bool wrote = false;
    const std::string payload = "handshake-first-stream";

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    server.setOnConnected([&](Connection::Ptr conn) {
        serverConnected = true;
        conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
            REQUIRE(stream != nullptr);
            serverStreamCreated = true;
        });
    });

    client.setOnConnected([&](Connection::Ptr conn) {
        clientConnected = true;
        const int32_t sid = conn->createStream(Connection::kStreamTypeBidirectional);
        REQUIRE(sid >= 0);
        eular::utp::Stream *stream = conn->getStream(static_cast<uint32_t>(sid));
        REQUIRE(stream != nullptr);
        const int32_t n = stream->write(payload.data(), payload.size(), false);
        REQUIRE(n == static_cast<int32_t>(payload.size()));
        wrote = true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 300;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    const bool ok = PumpUntil(
        loop,
        [&]() {
            return accepted && serverConnected && clientConnected && wrote && serverStreamCreated;
        },
        [&]() {
            if (!accepted && !server.m_pendingIncomingQueue.empty()) {
                accepted = (server.accept() == UTP_ERR_OK);
            }
        },
        500,
        1);

    REQUIRE(ok);
    REQUIRE(serverStreamCreated);
}

TEST_CASE("Context integration: pending buffers replay once after delayed HandshakeDone", "[Context][Integration][Handshake][Regression]")
{
    Config cfg;
    cfg.ack_every_n_packets = 10;
    cfg.handshake_timeout = 300;
    cfg.pending_pre_handshake_buffer_packets = 8;
    cfg.pending_pre_handshake_buffer_bytes = 4096;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    REQUIRE(ctx.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    const uint32_t localCid = 0x10002000;
    const uint32_t peerCid = 0x20001000;
    const Address peerAddr("127.0.0.1", 45454);

    ContextImpl::PendingIncomingConnection pending;
    pending.localCid = localCid;
    pending.peerCid = peerCid;
    pending.peerAddress = peerAddr;
    pending.peerIp = peerAddr.toIpString();
    pending.handshakeSent = true;
    pending.acceptStartUs = eular::utp::time::MonotonicMs() * 1000;
    pending.lastHandshakePacketNo = 42;
    pending.peerTp.init_max_streams_bidi = 8;
    pending.peerTp.init_max_streams_uni = 8;

    ctx.m_pendingIncoming.emplace(localCid, pending);

    auto buildStreamPayload = [&](uint64_t offset, char byte, bool withHandshakeDone) {
        FrameStream frame;
        frame.stream_id = 0;
        frame.stream_offset = offset;
        frame.stream_data_length = 1;
        frame.stream_data = &byte;

        std::vector<uint8_t> payload(static_cast<size_t>(frame.frameSize()), 0);
        const int32_t frameLen = frame.encode(payload.data(), payload.size());
        REQUIRE(frameLen > 0);
        payload.resize(static_cast<size_t>(frameLen));
        if (withHandshakeDone) {
            FrameHandshakeDone done;
            done.ack_handshake_pn = pending.lastHandshakePacketNo;
            const size_t oldSize = payload.size();
            payload.resize(oldSize + FRAME_HANDSHAKE_DONE_SIZE, 0);
            REQUIRE(done.encode(payload.data() + oldSize, FRAME_HANDSHAKE_DONE_SIZE) == FRAME_HANDSHAKE_DONE_SIZE);
        }
        return payload;
    };

    const std::vector<uint8_t> payloadA = buildStreamPayload(0, 'A', false);
    const std::vector<uint8_t> payloadBWithDone = buildStreamPayload(1, 'B', true);

    const std::vector<uint8_t> packetA = BuildRawPacket(peerCid, localCid, 10, UTP_TYPE_CTRL, payloadA);
    const std::vector<uint8_t> packetADuplicate = BuildRawPacket(peerCid, localCid, 10, UTP_TYPE_CTRL, payloadA);
    const std::vector<uint8_t> packetBWithDone = BuildRawPacket(peerCid, localCid, 11, UTP_TYPE_CTRL, payloadBWithDone);

    auto deliverToPendingPath = [&](const std::vector<uint8_t> &packetBytes) {
        auto pendingIt = ctx.m_pendingIncoming.find(localCid);
        REQUIRE(pendingIt != ctx.m_pendingIncoming.end());

        UdpSocket::MsgMetaInfo msg{};
        msg.data = packetBytes.data();
        msg.len = packetBytes.size();
        msg.metaInfo.peerAddress = peerAddr;

        auto packetReleaser = [&](PacketIn *pkt) {
            ctx.m_mm.putPacketIn(pkt);
        };
        std::unique_ptr<PacketIn, decltype(packetReleaser)> decoded(
            ctx.m_mm.getPacketIn(static_cast<uint32_t>(packetBytes.size())), packetReleaser);
        REQUIRE(decoded != nullptr);

        const bool decodeOk = ctx.decodeIncomingPendingPacket(msg, pendingIt->second, *decoded);
        REQUIRE(decodeOk);

        if (!decoded->hasFrame(eular::utp::kFrameHandshakeDone)) {
            pendingIt->second.bufferedBeforeHandshakeDone.emplace_back(packetBytes);
            pendingIt->second.bufferedBeforeHandshakeDoneBytes += packetBytes.size();
            return;
        }

        ContextImpl::PendingIncomingConnection snapshot = pendingIt->second;
        Context::ConnectInfo info;
        info.ip = snapshot.peerIp;
        info.port = snapshot.peerAddress.port();
        info.timeout = cfg.handshake_timeout;
        info.encrypted = snapshot.encrypted;

        ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(&ctx, &ctx.m_udpSocket, snapshot.localCid);
        REQUIRE(conn->initPassive(info,
                                  snapshot.peerAddress,
                                  snapshot.peerCid,
                                  snapshot.peerTp,
                                  snapshot.hasPeerAckFrequency ? &snapshot.peerAckFrequency : nullptr,
                                  snapshot.x25519,
                                  snapshot.aesCtx) == UTP_ERR_OK);
        REQUIRE(ctx.m_connections.emplace(snapshot.localCid, conn).second);

        ctx.removePendingIncoming(localCid);

        for (const auto &cached : snapshot.bufferedBeforeHandshakeDone) {
            UdpSocket::MsgMetaInfo replay{};
            replay.data = cached.data();
            replay.len = cached.size();
            replay.metaInfo.peerAddress = peerAddr;
            conn->onUdpPacket(replay);
        }
        conn->onUdpPacket(msg);
    };

    // Simulate delayed HandshakeDone with out-of-order and duplicate pre-handshake packets.
    deliverToPendingPath(packetA);
    deliverToPendingPath(packetADuplicate);
    deliverToPendingPath(packetBWithDone);

    auto connIt = ctx.m_connections.find(localCid);
    REQUIRE(connIt != ctx.m_connections.end());
    REQUIRE(connIt->second != nullptr);
    REQUIRE(connIt->second->m_receiveHistory.largest() == 11);
    REQUIRE(connIt->second->m_ackElicitingSinceLastAck == 0);

    auto streamIt = connIt->second->m_streams.find(0);
    REQUIRE(streamIt != connIt->second->m_streams.end());
    REQUIRE(streamIt->second != nullptr);

    std::array<char, 8> readBuf{};
    const int32_t nread = streamIt->second->read(readBuf.data(), readBuf.size());
    REQUIRE(nread == 2);
    REQUIRE(std::string(readBuf.data(), static_cast<size_t>(nread)) == "AB");
}

TEST_CASE("Context integration: pending handshake retry count is capped by config", "[Context][Integration][Handshake][Config]")
{
    Config cfg;
    cfg.handshake_timeout = 1000;
    cfg.pending_handshake_retry_interval_ms = 1;
    cfg.pending_handshake_max_retries = 2;

    ev::EventLoop loop;
    ContextImpl ctx(loop.loop(), &cfg);
    REQUIRE(ctx.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    const uint32_t localCid = 0x30004000;
    ContextImpl::PendingIncomingConnection pending;
    pending.localCid = localCid;
    pending.peerCid = 0x40003000;
    pending.peerAddress = Address("127.0.0.1", 9);
    pending.peerIp = pending.peerAddress.toIpString();
    pending.acceptStartUs = eular::utp::time::MonotonicUs() - 900000;
    pending.handshakeSent = true;
    pending.lastHandshakeSentUs = 0;

    ctx.m_pendingIncoming.emplace(localCid, pending);
    ctx.m_waitHandshakeDone.insert(localCid);

    for (int32_t i = 0; i < 6; ++i) {
        auto it = ctx.m_pendingIncoming.find(localCid);
        REQUIRE(it != ctx.m_pendingIncoming.end());
        it->second.acceptStartUs = eular::utp::time::MonotonicUs() - 900000;
        it->second.lastHandshakeSentUs = 0;
        ctx.processPendingHandshakeTimeouts();
    }

    auto it = ctx.m_pendingIncoming.find(localCid);
    REQUIRE(it != ctx.m_pendingIncoming.end());
    REQUIRE(it->second.handshakeRetryCount == cfg.pending_handshake_max_retries);
}

TEST_CASE("Context integration: stream OnWritable fires again after queued data drains", "[Context][Integration][Stream]")
{
    Config cfg;
    cfg.handshake_timeout = 200;

    ev::EventLoop loop;
    ContextImpl server(loop.loop(), &cfg);
    ContextImpl client(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == UTP_ERR_OK);
    REQUIRE(client.bind("127.0.0.1", 0, "") == UTP_ERR_OK);

    server.setOnNewConnection([](const Context::NewConnectionInfo &) {
        return true;
    });

    Context::ConnectInfo info;
    info.ip = "127.0.0.1";
    info.port = BoundPort(server);
    info.timeout = 200;
    REQUIRE(client.connect(info) == UTP_ERR_OK);

    REQUIRE(PumpUntil(
        loop,
        [&]() {
            return FindConnectedByRemote(server, BoundPort(client)) != nullptr
                && FindConnectedByRemote(client, BoundPort(server)) != nullptr;
        },
        [&]() {
            if (!server.m_pendingIncomingQueue.empty()) {
                (void)server.accept();
            }
        },
        300,
        1));

    ConnectionImpl::SP clientConn = FindConnectedByRemote(client, BoundPort(server));
    REQUIRE(clientConn != nullptr);

    const int32_t sid = clientConn->createStream(Connection::kStreamTypeBidirectional);
    REQUIRE(sid >= 0);

    auto streamIt = clientConn->m_streams.find(static_cast<uint32_t>(sid));
    REQUIRE(streamIt != clientConn->m_streams.end());
    REQUIRE(streamIt->second != nullptr);

    eular::utp::StreamImpl::SP stream = streamIt->second;
    REQUIRE(stream->writable());

    // Ensure stream flush path is not blocked by handshake barrier in this test.
    clientConn->onHandshakeDoneFrameAcked();

    int writableNotified = 0;
    stream->setOnWritable([&]() {
        ++writableNotified;
    });
    REQUIRE(writableNotified == 1);

    static const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    REQUIRE(stream->m_sendBuffer.write(payload, sizeof(payload)) == sizeof(payload));
    stream->m_sendQueuedBytes = eular::utp::StreamImpl::kDefaultBufferCapacity;

    REQUIRE_FALSE(stream->writable());

    clientConn->flushPendingStreamWrites();

    REQUIRE(stream->m_sendQueuedBytes == 0);
    REQUIRE(stream->writable());
    REQUIRE(writableNotified == 2);
}
