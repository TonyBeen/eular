#define CATCH_CONFIG_MAIN
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch.hpp>
#include <event2/event.h>

extern "C" {
#include "context/event_loop.h"
#include "proto/ack.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "proto/wire.h"
#include "socket/address.h"
#include "socket/udp.h"
#include "util/error.h"
#include "util/time.h"
}

TEST_CASE("wire cursors encode and decode big endian integers", "[wire]")
{
    const std::array<uint8_t, 15> expected = {
        0x12u, 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xdeu, 0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xabu, 0xcdu, 0xefu,
    };
    std::array<uint8_t, 15> encoded = {};
    utp_wire_writer_t       writer  = {};
    utp_wire_reader_t       reader  = {};
    uint8_t                 value8  = 0u;
    uint16_t                value16 = 0u;
    uint32_t                value32 = 0u;
    uint64_t                value64 = 0u;

    REQUIRE(utp_wire_writer_init(&writer, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UINT8_C(0x12)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u16(&writer, UINT16_C(0x3456)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u32(&writer, UINT32_C(0x789abcde)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u64(&writer, UINT64_C(0x0123456789abcdef)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(writer.remaining == 0u);
    REQUIRE(std::memcmp(encoded.data(), expected.data(), encoded.size()) == 0);
    REQUIRE(utp_wire_write_u8(&writer, 0u) == UTP_INTERNAL_ERROR_OVERFLOW);

    REQUIRE(utp_wire_reader_init(&reader, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_read_u8(&reader, &value8) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_read_u16(&reader, &value16) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_read_u32(&reader, &value32) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_read_u64(&reader, &value64) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(value8 == UINT8_C(0x12));
    REQUIRE(value16 == UINT16_C(0x3456));
    REQUIRE(value32 == UINT32_C(0x789abcde));
    REQUIRE(value64 == UINT64_C(0x0123456789abcdef));
    REQUIRE(utp_wire_read_u8(&reader, &value8) == UTP_INTERNAL_ERROR_OVERFLOW);
}

static uint64_t test_clock_now(void* user_data) { return *static_cast<uint64_t*>(user_data); }

struct test_event_probe {
    uint32_t events;
    size_t   calls;
};

static void test_event_probe_callback(uint32_t events, void* user_data)
{
    auto* probe = static_cast<test_event_probe*>(user_data);

    probe->events |= events;
    ++probe->calls;
}

TEST_CASE("clock accepts a deterministic test source", "[time]")
{
    uint64_t          now   = UINT64_C(1234567);
    const utp_clock_t clock = {test_clock_now, &now};

    REQUIRE(utp_clock_now_us(&clock) == now);
    now = UINT64_C(7654321);
    REQUIRE(utp_clock_now_us(&clock) == now);
}

TEST_CASE("address parses IPv4 and IPv6 without allocation", "[address]")
{
    utp_address_t ipv4      = {};
    utp_address_t ipv6      = {};
    utp_address_t same_ipv4 = {};

    REQUIRE(utp_address_parse(&ipv4, "192.0.2.1", 443u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(ipv4.family == UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(ipv4.port == 443u);
    REQUIRE(ipv4.address[0] == 192u);
    REQUIRE(ipv4.address[3] == 1u);
    REQUIRE(utp_address_parse(&ipv6, "2001:db8::1", 443u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(ipv6.family == UTP_ADDRESS_FAMILY_IPV6);
    REQUIRE(utp_address_parse(&same_ipv4, "192.0.2.1", 443u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_equal(&ipv4, &same_ipv4));
    REQUIRE_FALSE(utp_address_equal(&ipv4, &ipv6));
    REQUIRE(utp_address_parse(&same_ipv4, "not-an-address", 443u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("address recognizes only the IPv6 unspecified address", "[address]")
{
    utp_address_t unspecified = {};
    utp_address_t loopback    = {};
    utp_address_t ipv4        = {};

    REQUIRE(utp_address_parse(&unspecified, "::", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_parse(&loopback, "::1", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_parse(&ipv4, "0.0.0.0", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_is_unspecified_ipv6(&unspecified));
    REQUIRE_FALSE(utp_address_is_unspecified_ipv6(&loopback));
    REQUIRE_FALSE(utp_address_is_unspecified_ipv6(&ipv4));
}

TEST_CASE("address preserves the IPv6 scope identifier through sockaddr", "[address]")
{
    sockaddr_storage storage        = {};
    sockaddr_in6*    socket_address = reinterpret_cast<sockaddr_in6*>(&storage);
    utp_address_t    parsed         = {};
    utp_address_t    round_trip     = {};
    size_t           length         = 0u;

    REQUIRE(utp_address_parse(&parsed, "fe80::1", 7777u) == UTP_INTERNAL_ERROR_OK);
    parsed.scope_id = 7u;
    REQUIRE(utp_address_to_sockaddr(&parsed, &storage, &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == sizeof(sockaddr_in6));
    REQUIRE(socket_address->sin6_family == AF_INET6);
    REQUIRE(socket_address->sin6_scope_id == 7u);
    REQUIRE(utp_address_from_sockaddr(&round_trip, reinterpret_cast<const sockaddr*>(&storage), length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_equal(&parsed, &round_trip));
}

TEST_CASE("udp socket binds a nonblocking IPv4 loopback port", "[udp]")
{
    utp_address_t    requested = {};
    utp_address_t    local     = {};
    utp_udp_socket_t socket    = {};

    REQUIRE(utp_address_parse(&requested, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&socket);
    REQUIRE_FALSE(utp_udp_socket_is_open(&socket));
    REQUIRE(utp_udp_socket_open(&socket, requested.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_is_open(&socket));
    REQUIRE(utp_udp_socket_bind(&socket, &requested, nullptr, &local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(local.family == UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(local.port != 0u);
    utp_udp_socket_close(&socket);
    REQUIRE_FALSE(utp_udp_socket_is_open(&socket));
    utp_udp_socket_close(&socket);
}

TEST_CASE("udp socket enables address reuse without reuse port", "[udp]")
{
    utp_udp_socket_t socket        = {};
    int              reuse_address = 0;

    utp_udp_socket_init(&socket);
    REQUIRE(utp_udp_socket_open(&socket, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
#if defined(_WIN32)
    {
        int option_length = (int)sizeof(reuse_address);

        REQUIRE(getsockopt((SOCKET)socket.native_handle, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_address,
                           &option_length) == 0);
    }
#else
    {
        socklen_t option_length = (socklen_t)sizeof(reuse_address);

        REQUIRE(getsockopt((int)socket.native_handle, SOL_SOCKET, SO_REUSEADDR, &reuse_address, &option_length) == 0);
    }
#endif
    REQUIRE(reuse_address != 0);
    utp_udp_socket_close(&socket);
}

TEST_CASE("udp socket enables IPv6-only mode for any IPv6 bind", "[udp]")
{
    utp_address_t    requested = {};
    utp_address_t    local     = {};
    utp_udp_socket_t socket    = {};
    int              ipv6_only = 0;

    REQUIRE(utp_address_parse(&requested, "::", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&socket);
    REQUIRE(utp_udp_socket_open(&socket, UTP_ADDRESS_FAMILY_IPV6) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&socket, &requested, nullptr, &local) == UTP_INTERNAL_ERROR_OK);
#if defined(_WIN32)
    {
        int option_length = (int)sizeof(ipv6_only);

        REQUIRE(getsockopt((SOCKET)socket.native_handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&ipv6_only,
                           &option_length) == 0);
    }
#else
    {
        socklen_t option_length = (socklen_t)sizeof(ipv6_only);

        REQUIRE(getsockopt((int)socket.native_handle, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, &option_length) == 0);
    }
#endif
    REQUIRE(ipv6_only != 0);
    utp_udp_socket_close(&socket);
}

TEST_CASE("event loop dispatches a readable UDP socket", "[event][udp]")
{
    const std::array<uint8_t, 1> payload        = {UINT8_C(0x42)};
    utp_address_t                loopback       = {};
    utp_address_t                sender_local   = {};
    utp_address_t                receiver_local = {};
    utp_event_loop_t             loop           = {};
    utp_event_t                  event          = {};
    event_base*                  native_base    = event_base_new();
    utp_udp_socket_t             sender         = {};
    utp_udp_socket_t             receiver       = {};
    test_event_probe             probe          = {};
    size_t                       sent_length    = 0u;

    REQUIRE(native_base != nullptr);
    REQUIRE(utp_event_loop_init(&loop, native_base, nullptr, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_parse(&loopback, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    utp_event_init(&event);
    REQUIRE(utp_udp_socket_open(&sender, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_add_udp(&loop, &event, &receiver, UTP_EVENT_READABLE, true, test_event_probe_callback, &probe) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_loop_run_once(&loop, true) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(probe.calls == 0u);
    REQUIRE(utp_udp_socket_send_to(&sender, payload.data(), payload.size(), &receiver_local, &sent_length) ==
            UTP_INTERNAL_ERROR_OK);

    for (size_t attempt = 0u; attempt < 1000u && probe.calls == 0u; ++attempt) {
        REQUIRE(utp_event_loop_run_once(&loop, true) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(probe.calls == 1u);
    REQUIRE((probe.events & UTP_EVENT_READABLE) != 0u);

    utp_event_remove(&event);
    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
    utp_event_loop_close(&loop);
    event_base_free(native_base);
}

TEST_CASE("event loop dispatches a zero-delay one-shot timer", "[event]")
{
    utp_event_loop_t loop        = {};
    utp_event_t      event       = {};
    test_event_probe probe       = {};
    event_base*      native_base = event_base_new();

    REQUIRE(native_base != nullptr);
    REQUIRE(utp_event_loop_init(&loop, native_base, nullptr, nullptr) == UTP_INTERNAL_ERROR_OK);
    utp_event_init(&event);
    REQUIRE(utp_event_add_timer(&loop, &event, 0u, false, test_event_probe_callback, &probe) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_loop_run_once(&loop, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(probe.calls == 1u);
    REQUIRE((probe.events & UTP_EVENT_TIMEOUT) != 0u);
    utp_event_remove(&event);
    utp_event_loop_close(&loop);
    event_base_free(native_base);
}

TEST_CASE("event loop rearms a pending timer without replacing its callback", "[event]")
{
    utp_event_loop_t loop        = {};
    utp_event_t      event       = {};
    test_event_probe probe       = {};
    event_base*      native_base = event_base_new();

    REQUIRE(native_base != nullptr);
    REQUIRE(utp_event_loop_init(&loop, native_base, nullptr, nullptr) == UTP_INTERNAL_ERROR_OK);
    utp_event_init(&event);
    REQUIRE(utp_event_add_timer(&loop, &event, UINT64_C(1000000), false, test_event_probe_callback, &probe) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_reset_timer(&event, 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_loop_run_once(&loop, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(probe.calls == 1u);
    REQUIRE((probe.events & UTP_EVENT_TIMEOUT) != 0u);
    utp_event_remove(&event);
    utp_event_loop_close(&loop);
    event_base_free(native_base);
}

TEST_CASE("udp socket sends a datagram and reports its peer", "[udp]")
{
    const std::array<uint8_t, 5>        payload         = {'h', 'e', 'l', 'l', 'o'};
    std::array<uint8_t, payload.size()> received        = {};
    utp_address_t                       loopback        = {};
    utp_address_t                       sender_local    = {};
    utp_address_t                       receiver_local  = {};
    utp_address_t                       peer            = {};
    utp_udp_socket_t                    sender          = {};
    utp_udp_socket_t                    receiver        = {};
    utp_internal_error_t                receive_error   = UTP_INTERNAL_ERROR_WOULD_BLOCK;
    size_t                              sent_length     = 0u;
    size_t                              received_length = 0u;

    REQUIRE(utp_address_parse(&loopback, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    REQUIRE(utp_udp_socket_open(&sender, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_udp_socket_recv_from(&receiver, received.data(), received.size(), &received_length, &peer) ==
            UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(utp_udp_socket_send_to(&sender, payload.data(), payload.size(), &receiver_local, &sent_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == payload.size());

    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        receive_error = utp_udp_socket_recv_from(&receiver, received.data(), received.size(), &received_length, &peer);
        if (receive_error == UTP_INTERNAL_ERROR_OK) {
            break;
        }
        REQUIRE(receive_error == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    }
    REQUIRE(receive_error == UTP_INTERNAL_ERROR_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(std::memcmp(received.data(), payload.data(), payload.size()) == 0);
    REQUIRE(utp_address_equal(&peer, &sender_local));

    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
}

#if defined(__APPLE__) || defined(__linux__)
static void test_udp_socket_reply_from_received_local(const char* loopback_text, size_t address_length)
{
    const std::array<uint8_t, 4>        request         = {'p', 'i', 'n', 'g'};
    const std::array<uint8_t, 4>        response        = {'p', 'o', 'n', 'g'};
    std::array<uint8_t, request.size()> received        = {};
    utp_address_t                       loopback        = {};
    utp_address_t                       sender_local    = {};
    utp_address_t                       receiver_local  = {};
    utp_address_t                       request_peer    = {};
    utp_address_t                       request_local   = {};
    utp_address_t                       response_peer   = {};
    utp_address_t                       response_local  = {};
    utp_udp_socket_t                    sender          = {};
    utp_udp_socket_t                    receiver        = {};
    utp_internal_error_t                receive_error   = UTP_INTERNAL_ERROR_WOULD_BLOCK;
    size_t                              sent_length     = 0u;
    size_t                              received_length = 0u;

    REQUIRE(utp_address_parse(&loopback, loopback_text, 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    REQUIRE(utp_udp_socket_open(&sender, loopback.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, loopback.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_send_to(&sender, request.data(), request.size(), &receiver_local, &sent_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == request.size());

    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        receive_error = utp_udp_socket_recv_from_ex(&receiver, received.data(), received.size(), &received_length,
                                                    &request_peer, &request_local);
        if (receive_error == UTP_INTERNAL_ERROR_OK) {
            break;
        }
        REQUIRE(receive_error == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    }
    REQUIRE(receive_error == UTP_INTERNAL_ERROR_OK);
    REQUIRE(received_length == request.size());
    REQUIRE(std::memcmp(received.data(), request.data(), request.size()) == 0);
    REQUIRE(utp_address_equal(&request_peer, &sender_local));
    REQUIRE(request_local.family == receiver_local.family);
    REQUIRE(request_local.port == receiver_local.port);
    REQUIRE(std::memcmp(request_local.address, receiver_local.address, address_length) == 0);

    REQUIRE(utp_udp_socket_send_from_to(&receiver, response.data(), response.size(), &request_peer, &request_local,
                                        &sent_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == response.size());
    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        receive_error = utp_udp_socket_recv_from_ex(&sender, received.data(), received.size(), &received_length,
                                                    &response_peer, &response_local);
        if (receive_error == UTP_INTERNAL_ERROR_OK) {
            break;
        }
        REQUIRE(receive_error == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    }
    REQUIRE(receive_error == UTP_INTERNAL_ERROR_OK);
    REQUIRE(received_length == response.size());
    REQUIRE(std::memcmp(received.data(), response.data(), response.size()) == 0);
    REQUIRE(utp_address_equal(&response_peer, &receiver_local));
    REQUIRE(response_local.family == sender_local.family);
    REQUIRE(response_local.port == sender_local.port);
    REQUIRE(std::memcmp(response_local.address, sender_local.address, address_length) == 0);

    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
}

TEST_CASE("udp socket preserves an IPv4 local address for a reply", "[udp]")
{
    test_udp_socket_reply_from_received_local("127.0.0.1", 4u);
}

TEST_CASE("udp socket preserves an IPv6 local address for a reply", "[udp]")
{
    test_udp_socket_reply_from_received_local("::1", 16u);
}
#endif

TEST_CASE("udp socket sends a datagram from slices", "[udp]")
{
    const std::array<uint8_t, 2> first           = {UINT8_C(0xaa), UINT8_C(0xbb)};
    const std::array<uint8_t, 3> second          = {UINT8_C(0xcc), UINT8_C(0xdd), UINT8_C(0xee)};
    std::array<uint8_t, 8>       received        = {};
    utp_address_t                loopback        = {};
    utp_address_t                sender_local    = {};
    utp_address_t                receiver_local  = {};
    utp_address_t                peer            = {};
    utp_udp_socket_t             sender          = {};
    utp_udp_socket_t             receiver        = {};
    utp_internal_error_t         receive_error   = UTP_INTERNAL_ERROR_WOULD_BLOCK;
    size_t                       sent_length     = 0u;
    size_t                       received_length = 0u;
    const utp_udp_send_slice_t   slices[]        = {{first.data(), first.size()}, {second.data(), second.size()}};

    REQUIRE(utp_address_parse(&loopback, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    REQUIRE(utp_udp_socket_open(&sender, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_udp_socket_send_to_slices(&sender, slices, 2u, &receiver_local, &sent_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == first.size() + second.size());
    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        receive_error = utp_udp_socket_recv_from(&receiver, received.data(), received.size(), &received_length, &peer);
        if (receive_error == UTP_INTERNAL_ERROR_OK) {
            break;
        }
        REQUIRE(receive_error == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    }
    REQUIRE(receive_error == UTP_INTERNAL_ERROR_OK);
    REQUIRE(received_length == first.size() + second.size());
    REQUIRE(received[0] == first[0]);
    REQUIRE(received[1] == first[1]);
    REQUIRE(received[2] == second[0]);
    REQUIRE(received[3] == second[1]);
    REQUIRE(received[4] == second[2]);
    REQUIRE(utp_address_equal(&peer, &sender_local));

    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
}

#if defined(__linux__) && defined(UTP_HAVE_SENDMMSG) && defined(UTP_HAVE_RECVMMSG)
static void test_udp_socket_batch(const char* loopback_text, size_t address_length)
{
    const std::array<uint8_t, 3>          first  = {UINT8_C(0x11), UINT8_C(0x12), UINT8_C(0x13)};
    const std::array<uint8_t, 2>          second = {UINT8_C(0x21), UINT8_C(0x22)};
    const std::array<uint8_t, 4>          third  = {UINT8_C(0x31), UINT8_C(0x32), UINT8_C(0x33), UINT8_C(0x34)};
    const std::array<size_t, 3>           expected_lengths = {first.size(), second.size(), third.size()};
    const utp_udp_send_slice_t            first_slice      = {first.data(), first.size()};
    const utp_udp_send_slice_t            second_slice     = {second.data(), second.size()};
    const utp_udp_send_slice_t            third_slice      = {third.data(), third.size()};
    std::array<std::array<uint8_t, 8>, 3> received         = {};
    utp_udp_send_message_t                sends[3]         = {};
    utp_address_t                         loopback         = {};
    utp_address_t                         sender_local     = {};
    utp_address_t                         receiver_local   = {};
    utp_udp_socket_t                      sender           = {};
    utp_udp_socket_t                      receiver         = {};
    size_t                                sent_count       = 0u;
    size_t                                received_total   = 0u;

    sends[0].slices      = &first_slice;
    sends[0].peer        = &receiver_local;
    sends[0].slice_count = 1u;
    sends[1].slices      = &second_slice;
    sends[1].peer        = &receiver_local;
    sends[1].slice_count = 1u;
    sends[2].slices      = &third_slice;
    sends[2].peer        = &receiver_local;
    sends[2].slice_count = 1u;
    REQUIRE(utp_address_parse(&loopback, loopback_text, 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    REQUIRE(utp_udp_socket_open(&sender, loopback.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, loopback.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_send_messages(&sender, sends, 3u, &sent_count) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_count == 3u);
    REQUIRE(sends[0].sent_length == first.size());
    REQUIRE(sends[1].sent_length == second.size());
    REQUIRE(sends[2].sent_length == third.size());

    for (size_t attempt = 0u; attempt < 1000u && received_total < received.size(); ++attempt) {
        utp_udp_receive_message_t messages[3] = {};
        size_t                    received_count;
        utp_internal_error_t      error;

        for (size_t index = 0u; index < received.size() - received_total; ++index) {
            messages[index].data     = received[received_total + index].data();
            messages[index].capacity = received[received_total + index].size();
        }
        error = utp_udp_socket_receive_messages(&receiver, messages, received.size() - received_total, &received_count);
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            continue;
        }
        REQUIRE(error == UTP_INTERNAL_ERROR_OK);
        REQUIRE(received_count > 0u);
        for (size_t index = 0u; index < received_count; ++index) {
            REQUIRE(messages[index].error == UTP_INTERNAL_ERROR_OK);
            REQUIRE(messages[index].received_length == expected_lengths[received_total + index]);
            REQUIRE(utp_address_equal(&messages[index].peer, &sender_local));
            REQUIRE(messages[index].local.family == receiver_local.family);
            REQUIRE(messages[index].local.port == receiver_local.port);
            REQUIRE(std::memcmp(messages[index].local.address, receiver_local.address, address_length) == 0);
        }
        received_total += received_count;
    }
    REQUIRE(received_total == received.size());
    REQUIRE(std::memcmp(received[0].data(), first.data(), first.size()) == 0);
    REQUIRE(std::memcmp(received[1].data(), second.data(), second.size()) == 0);
    REQUIRE(std::memcmp(received[2].data(), third.data(), third.size()) == 0);

    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
}

TEST_CASE("udp socket batches IPv4 datagrams with independent local addresses", "[udp]")
{
    test_udp_socket_batch("127.0.0.1", 4u);
}

TEST_CASE("udp socket batches IPv6 datagrams with independent local addresses", "[udp]")
{
    test_udp_socket_batch("::1", 16u);
}
#endif

TEST_CASE("udp socket rejects a datagram that exceeds receive capacity", "[udp]")
{
    const std::array<uint8_t, 6> payload = {'o', 'v', 'e', 'r', 'f', 'l'};
    struct {
        std::array<uint8_t, 5> bytes;
        uint8_t                guard;
    } received                           = {{}, UINT8_C(0xa5)};
    utp_address_t        loopback        = {};
    utp_address_t        sender_local    = {};
    utp_address_t        receiver_local  = {};
    utp_address_t        peer            = {};
    utp_udp_socket_t     sender          = {};
    utp_udp_socket_t     receiver        = {};
    utp_internal_error_t receive_error   = UTP_INTERNAL_ERROR_WOULD_BLOCK;
    size_t               sent_length     = 0u;
    size_t               received_length = 1u;

    REQUIRE(utp_address_parse(&loopback, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&sender);
    utp_udp_socket_init(&receiver);
    REQUIRE(utp_udp_socket_open(&sender, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&receiver, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&sender, &loopback, nullptr, &sender_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&receiver, &loopback, nullptr, &receiver_local) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_send_to(&sender, payload.data(), payload.size(), &receiver_local, &sent_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == payload.size());

    for (size_t attempt = 0u; attempt < 1000u; ++attempt) {
        receive_error =
            utp_udp_socket_recv_from(&receiver, received.bytes.data(), received.bytes.size(), &received_length, &peer);
        if (receive_error == UTP_INTERNAL_ERROR_OVERFLOW) {
            break;
        }
        REQUIRE(receive_error == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    }
    REQUIRE(receive_error == UTP_INTERNAL_ERROR_OVERFLOW);
    REQUIRE(received_length == 0u);
    REQUIRE(received.guard == UINT8_C(0xa5));

    utp_udp_socket_close(&receiver);
    utp_udp_socket_close(&sender);
}

TEST_CASE("packet header encodes in network byte order", "[proto]")
{
    const utp_packet_header_t expected = {
        UINT32_C(0x12345678), UINT32_C(0x9abcdef0),    UINT64_C(0x0123456789abcdef),
        UINT16_C(0x1234),     UTP_PACKET_TYPE_INITIAL, UINT8_C(0xa5),
    };
    const std::array<uint8_t, UTP_PACKET_HEADER_SIZE> expected_bytes = {
        0x12u,
        0x34u,
        0x56u,
        0x78u,
        0x9au,
        0xbcu,
        0xdeu,
        0xf0u,
        0x01u,
        0x23u,
        0x45u,
        0x67u,
        0x89u,
        0xabu,
        0xcdu,
        0xefu,
        0x12u,
        0x34u,
        UTP_PACKET_TYPE_INITIAL,
        0xa5u,
    };
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE> encoded = {};
    utp_packet_header_t                         decoded = {};

    REQUIRE(utp_proto_encode_header(encoded.data(), encoded.size(), &expected) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), expected_bytes.data(), encoded.size()) == 0);
    REQUIRE(utp_proto_decode_header(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.scid == expected.scid);
    REQUIRE(decoded.dcid == expected.dcid);
    REQUIRE(decoded.packet_number == expected.packet_number);
    REQUIRE(decoded.payload_length == expected.payload_length);
    REQUIRE(decoded.type == expected.type);
    REQUIRE(decoded.reserve == expected.reserve);
}

TEST_CASE("packet header rejects truncated buffers", "[proto]")
{
    const utp_packet_header_t header = {
        UINT32_C(1), UINT32_C(2), UINT64_C(3), UINT16_C(0), UTP_PACKET_TYPE_CTRL, UINT8_C(0),
    };
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE> encoded = {};
    utp_packet_header_t                         decoded = {};

    REQUIRE(utp_proto_encode_header(encoded.data(), encoded.size() - 1u, &header) == UTP_INTERNAL_ERROR_OVERFLOW);
    REQUIRE(utp_proto_decode_header(&decoded, encoded.data(), encoded.size() - 1u) == UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("packet header rejects out of range packet numbers", "[proto]")
{
    const utp_packet_header_t invalid = {
        UINT32_C(1), UINT32_C(2), UTP_PACKET_NUMBER_MAX + UINT64_C(1), UINT16_C(0), UTP_PACKET_TYPE_HANDSHAKE,
        UINT8_C(0),
    };
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE> encoded = {};
    utp_packet_header_t                         decoded = {};

    REQUIRE(utp_proto_encode_header(encoded.data(), encoded.size(), &invalid) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    encoded[8] = UINT8_C(0x40);
    REQUIRE(utp_proto_decode_header(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("frame length covers every supported wire frame", "[frame]")
{
    struct frame_case {
        uint8_t type;
        size_t  length;
    };
    const std::array<frame_case, 22> cases = {{{UTP_FRAME_TYPE_STREAM, 16u},
                                               {UTP_FRAME_TYPE_ACK, 16u},
                                               {UTP_FRAME_TYPE_PADDING, 3u},
                                               {UTP_FRAME_TYPE_CONNECTION_CLOSE, 5u},
                                               {UTP_FRAME_TYPE_PING, 1u},
                                               {UTP_FRAME_TYPE_RESET_STREAM, 15u},
                                               {UTP_FRAME_TYPE_STREAMS_BLOCKED, 4u},
                                               {UTP_FRAME_TYPE_MAX_STREAMS, 4u},
                                               {UTP_FRAME_TYPE_PATH_CHALLENGE, 9u},
                                               {UTP_FRAME_TYPE_PATH_RESPONSE, 9u},
                                               {UTP_FRAME_TYPE_CRYPTO, 35u},
                                               {UTP_FRAME_TYPE_SESSION_TOKEN, 10u},
                                               {UTP_FRAME_TYPE_ACK_FREQUENCY, 7u},
                                               {UTP_FRAME_TYPE_VERSION, 5u},
                                               {UTP_FRAME_TYPE_HANDSHAKE_DONE, 9u},
                                               {UTP_FRAME_TYPE_TRANSPORT_PARAMS, 38u},
                                               {UTP_FRAME_TYPE_HANDSHAKE_DELAY, 5u},
                                               {UTP_FRAME_TYPE_MAX_DATA, 9u},
                                               {UTP_FRAME_TYPE_MAX_STREAM_DATA, 13u},
                                               {UTP_FRAME_TYPE_DATA_BLOCKED, 9u},
                                               {UTP_FRAME_TYPE_STREAM_DATA_BLOCKED, 13u},
                                               {UTP_FRAME_TYPE_STOP_SENDING, 7u}}};
    std::vector<uint8_t>             payload;
    uint32_t                         expected_types = 0u;

    for (const frame_case& test_case : cases) {
        std::vector<uint8_t> frame(test_case.length, 0u);
        size_t               measured_length = 0u;
        uint8_t              measured_type   = 0u;

        frame[0] = test_case.type;
        REQUIRE(utp_frame_measure(frame.data(), frame.size(), &measured_type, &measured_length) ==
                UTP_INTERNAL_ERROR_OK);
        REQUIRE(measured_type == test_case.type);
        REQUIRE(measured_length == test_case.length);
        payload.insert(payload.end(), frame.begin(), frame.end());
        expected_types |= UTP_FRAME_BIT(test_case.type);
    }

    uint32_t frame_types = 0u;
    REQUIRE(utp_frame_scan(payload.data(), payload.size(), &frame_types) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_types == expected_types);
}

TEST_CASE("transport parameter and ACK frequency frames normalize and validate values", "[frame]")
{
    const utp_frame_transport_params_t params = {
        UINT64_C(1048576),
        UINT64_C(262144),
        UINT64_C(131072),
        30000u,
        UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS,
        800u,
        64u,
        32u,
        3u,
    };
    const utp_frame_ack_frequency_t                      frequency         = {0u, 0u, 0u};
    std::array<uint8_t, UTP_FRAME_TRANSPORT_PARAMS_SIZE> params_bytes      = {};
    std::array<uint8_t, UTP_FRAME_ACK_FREQUENCY_SIZE>    frequency_bytes   = {};
    utp_frame_transport_params_t                         decoded_params    = {};
    utp_frame_ack_frequency_t                            decoded_frequency = {};

    REQUIRE(utp_frame_transport_params_encode(params_bytes.data(), params_bytes.size(), &params) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_transport_params_decode(&decoded_params, params_bytes.data(), params_bytes.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_params.initial_max_data == params.initial_max_data);
    REQUIRE(decoded_params.initial_max_stream_data_bidi_local == params.initial_max_stream_data_bidi_local);
    REQUIRE(decoded_params.initial_max_stream_data_bidi_remote == params.initial_max_stream_data_bidi_remote);

    REQUIRE(utp_frame_ack_frequency_encode(frequency_bytes.data(), frequency_bytes.size(), &frequency) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_ack_frequency_decode(&decoded_frequency, frequency_bytes.data(), frequency_bytes.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_frequency.ack_eliciting_threshold == 5u);
    REQUIRE(decoded_frequency.reordering_threshold == 3u);
    REQUIRE(decoded_frequency.max_ack_delay_ms == 25u);

    params_bytes[1] = 0x01u;
    params_bytes[2] = 0x00u;
    REQUIRE(utp_frame_transport_params_decode(&decoded_params, params_bytes.data(), params_bytes.size()) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_frame_transport_params_encode(params_bytes.data(), params_bytes.size() - 1u, &params) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);

    frequency_bytes[1] = 255u;
    frequency_bytes[2] = 255u;
    REQUIRE(utp_frame_ack_frequency_decode(&decoded_frequency, frequency_bytes.data(), frequency_bytes.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_frequency.ack_eliciting_threshold == UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD);
    REQUIRE(decoded_frequency.reordering_threshold == UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD);
}

TEST_CASE("session token frame keeps a borrowed token view", "[frame]")
{
    std::array<uint8_t, UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + 64u> buffer  = {};
    utp_frame_session_token_t                                      encoded = {};
    utp_frame_session_token_t                                      decoded = {};

    for (size_t index = 0u; index < 64u; ++index) {
        buffer[UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + index] = static_cast<uint8_t>(index);
    }
    encoded.payload            = buffer.data() + UTP_FRAME_SESSION_TOKEN_HEADER_SIZE;
    encoded.payload_length     = 64u;
    encoded.expires_at_seconds = UINT64_C(1710000000);
    REQUIRE(utp_frame_session_token_encode(buffer.data(), buffer.size(), &encoded) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_session_token_decode(&decoded, buffer.data(), buffer.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.payload_length == 64u);
    REQUIRE(decoded.expires_at_seconds == UINT64_C(1710000000));
    REQUIRE(decoded.payload == buffer.data() + UTP_FRAME_SESSION_TOKEN_HEADER_SIZE);
    REQUIRE(decoded.payload[63] == 63u);
}

TEST_CASE("frame length rejects unknown and truncated frames", "[frame]")
{
    const std::array<uint8_t, 1>  unknown          = {UTP_FRAME_TYPE_MAX};
    const std::array<uint8_t, 15> truncated_stream = {UTP_FRAME_TYPE_STREAM};
    size_t                        frame_length     = 0u;
    uint8_t                       frame_type       = 0u;

    REQUIRE(utp_frame_measure(unknown.data(), unknown.size(), &frame_type, &frame_length) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_frame_measure(truncated_stream.data(), truncated_stream.size(), &frame_type, &frame_length) ==
            UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("MAX_STREAMS and STREAMS_BLOCKED round trip", "[frame]")
{
    const utp_frame_streams_limit_t                   maximum = {UINT16_C(64), UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL};
    const utp_frame_streams_limit_t                   blocked = {UINT16_C(32), UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL};
    utp_frame_streams_limit_t                         decoded = {};
    std::array<uint8_t, UTP_FRAME_STREAMS_LIMIT_SIZE> encoded = {};

    REQUIRE(utp_frame_max_streams_encode(encoded.data(), encoded.size(), &maximum) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_max_streams_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.stream_type == maximum.stream_type);
    REQUIRE(decoded.stream_limit == maximum.stream_limit);

    REQUIRE(utp_frame_streams_blocked_encode(encoded.data(), encoded.size(), &blocked) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_streams_blocked_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.stream_type == blocked.stream_type);
    REQUIRE(decoded.stream_limit == blocked.stream_limit);

    encoded[1] = UINT8_C(2);
    REQUIRE(utp_frame_streams_blocked_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_PROTOCOL);
}

TEST_CASE("packet view validates frame layout without copying payload", "[frame]")
{
    const std::array<uint8_t, 6> payload = {
        UTP_FRAME_TYPE_PING, UTP_FRAME_TYPE_VERSION, 0u, 0u, 0u, 2u,
    };
    const utp_packet_header_t header = {
        UINT32_C(1), UINT32_C(2), UINT64_C(3), static_cast<uint16_t>(payload.size()), UTP_PACKET_TYPE_CTRL, 0u,
    };
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + payload.size()> packet       = {};
    utp_packet_view_t                                            view         = {};
    const uint8_t*                                               frame_data   = nullptr;
    size_t                                                       frame_length = 0u;
    size_t                                                       offset       = 0u;
    uint8_t                                                      frame_type   = 0u;

    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet.data() + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    REQUIRE(utp_packet_view_decode(&view, packet.data(), packet.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.payload == packet.data() + UTP_PACKET_HEADER_SIZE);
    REQUIRE(view.payload_length == payload.size());
    REQUIRE(view.frame_types == (UTP_FRAME_BIT(UTP_FRAME_TYPE_PING) | UTP_FRAME_BIT(UTP_FRAME_TYPE_VERSION)));
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_PING);
    REQUIRE(frame_length == 1u);
    REQUIRE(frame_data == view.payload);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_VERSION);
    REQUIRE(frame_length == 5u);
    REQUIRE(offset == payload.size());
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length) ==
            UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("ack frame encodes and decodes descending ranges", "[ack]")
{
    std::array<utp_ack_range_t, 2> ranges   = {{{100u, 105u}, {90u, 94u}}};
    const utp_ack_info_t           ack      = {105u, 288u, ranges.data(), ranges.size(), ranges.size()};
    const std::array<uint8_t, 24>  expected = {
        UTP_FRAME_TYPE_ACK,
        1u,
        0u,
        36u,
        0u,
        0u,
        0u,
        6u,
        0u,
        0u,
        0u,
        0u,
        0u,
        0u,
        0u,
        105u,
        0u,
        0u,
        0u,
        5u,
        0u,
        0u,
        0u,
        5u,
    };
    std::array<uint8_t, 24>        encoded        = {};
    std::array<utp_ack_range_t, 2> decoded_ranges = {};
    utp_ack_info_t                 decoded        = {0u, 0u, decoded_ranges.data(), 0u, decoded_ranges.size()};
    size_t                         encoded_length = 0u;
    size_t                         consumed       = 0u;

    REQUIRE(utp_ack_encode(encoded.data(), encoded.size(), &ack, 3u, &encoded_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(encoded_length == encoded.size());
    REQUIRE(std::memcmp(encoded.data(), expected.data(), encoded.size()) == 0);
    REQUIRE(utp_ack_decode(&decoded, encoded.data(), encoded.size(), 3u, &consumed) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(consumed == encoded.size());
    REQUIRE(decoded.largest_acked == 105u);
    REQUIRE(decoded.ack_delay == 288u);
    REQUIRE(decoded.range_count == 2u);
    REQUIRE(decoded.ranges[0].low == 100u);
    REQUIRE(decoded.ranges[0].high == 105u);
    REQUIRE(decoded.ranges[1].low == 90u);
    REQUIRE(decoded.ranges[1].high == 94u);
}

TEST_CASE("ack decoder rejects invalid ranges without changing output", "[ack]")
{
    const std::array<uint8_t, 16>  invalid  = {UTP_FRAME_TYPE_ACK};
    std::array<utp_ack_range_t, 1> ranges   = {{{7u, 8u}}};
    utp_ack_info_t                 decoded  = {8u, 9u, ranges.data(), 1u, ranges.size()};
    size_t                         consumed = 99u;

    REQUIRE(utp_ack_decode(&decoded, invalid.data(), invalid.size(), 0u, &consumed) == UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(consumed == 99u);
    REQUIRE(decoded.largest_acked == 8u);
    REQUIRE(decoded.ack_delay == 9u);
    REQUIRE(decoded.range_count == 1u);
    REQUIRE(decoded.ranges[0].low == 7u);
    REQUIRE(decoded.ranges[0].high == 8u);
}

TEST_CASE("ack decoder preserves output when a later range is malformed", "[ack]")
{
    const std::array<uint8_t, 24> encoded = {
        UTP_FRAME_TYPE_ACK, 1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 10u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u,
    };
    std::array<utp_ack_range_t, 2> ranges   = {{{30u, 31u}, {20u, 21u}}};
    utp_ack_info_t                 decoded  = {100u, 99u, ranges.data(), ranges.size(), ranges.size()};
    size_t                         consumed = 77u;

    REQUIRE(utp_ack_decode(&decoded, encoded.data(), encoded.size(), 0u, &consumed) == UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(consumed == 77u);
    REQUIRE(decoded.largest_acked == 100u);
    REQUIRE(decoded.ack_delay == 99u);
    REQUIRE(decoded.range_count == ranges.size());
    REQUIRE(decoded.ranges[0].low == 30u);
    REQUIRE(decoded.ranges[0].high == 31u);
    REQUIRE(decoded.ranges[1].low == 20u);
    REQUIRE(decoded.ranges[1].high == 21u);
}

TEST_CASE("ack decoder enforces the caller range capacity", "[ack]")
{
    const std::array<uint8_t, 24> encoded = {
        UTP_FRAME_TYPE_ACK, 1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 10u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u,
    };
    std::array<utp_ack_range_t, 1> ranges   = {};
    utp_ack_info_t                 decoded  = {0u, 0u, ranges.data(), 0u, ranges.size()};
    size_t                         consumed = 0u;

    REQUIRE(utp_ack_decode(&decoded, encoded.data(), encoded.size(), 0u, &consumed) == UTP_INTERNAL_ERROR_LIMIT);
}

TEST_CASE("version frame round trips with an exact fixed layout", "[frame]")
{
    const utp_frame_version_t                         expected = {UINT32_C(0x01020304)};
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> bytes    = {
        UTP_FRAME_TYPE_VERSION, 1u, 2u, 3u, 4u,
    };
    std::array<uint8_t, UTP_FRAME_VERSION_SIZE> encoded = {};
    utp_frame_version_t                         decoded = {};

    REQUIRE(utp_frame_version_encode(encoded.data(), encoded.size(), &expected) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), bytes.data(), bytes.size()) == 0);
    REQUIRE(utp_frame_version_decode(&decoded, bytes.data(), bytes.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.version == expected.version);
}

TEST_CASE("crypto frame round trips and rejects invalid negotiation fields", "[frame][crypto]")
{
    utp_frame_crypto_t                         expected = {};
    utp_frame_crypto_t                         decoded  = {};
    std::array<uint8_t, UTP_FRAME_CRYPTO_SIZE> encoded  = {};

    expected.crypto_type = UTP_FRAME_CRYPTO_TYPE_AES_GCM_256;
    for (size_t index = 0u; index < sizeof(expected.ephemeral_public_key); ++index) {
        expected.ephemeral_public_key[index] = static_cast<uint8_t>(index + 1u);
    }

    REQUIRE(utp_frame_crypto_encode(encoded.data(), encoded.size(), &expected) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(encoded[0] == UTP_FRAME_TYPE_CRYPTO);
    REQUIRE(encoded[1] == UTP_FRAME_CRYPTO_TYPE_AES_GCM_256);
    REQUIRE(encoded[2] == 0u);
    REQUIRE(std::memcmp(encoded.data() + 3u, expected.ephemeral_public_key, sizeof(expected.ephemeral_public_key)) ==
            0);
    REQUIRE(utp_frame_crypto_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.crypto_type == expected.crypto_type);
    REQUIRE(std::memcmp(decoded.ephemeral_public_key, expected.ephemeral_public_key,
                        sizeof(expected.ephemeral_public_key)) == 0);

    encoded[2] = 1u;
    REQUIRE(utp_frame_crypto_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_PROTOCOL);
    encoded[2] = 0u;
    encoded[1] = UTP_FRAME_CRYPTO_TYPE_AES_GCM_256 + 1u;
    REQUIRE(utp_frame_crypto_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_PROTOCOL);
    expected.crypto_type = UTP_FRAME_CRYPTO_TYPE_AES_GCM_256 + 1u;
    REQUIRE(utp_frame_crypto_encode(encoded.data(), encoded.size(), &expected) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("path and handshake done frames reject a mismatched type", "[frame]")
{
    const utp_frame_path_t                             path         = {{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}};
    const utp_frame_handshake_done_t                   done         = {UINT64_C(0x0102030405060708)};
    std::array<uint8_t, UTP_FRAME_PATH_SIZE>           path_bytes   = {};
    std::array<uint8_t, UTP_FRAME_HANDSHAKE_DONE_SIZE> done_bytes   = {};
    utp_frame_path_t                                   decoded_path = {};
    utp_frame_handshake_done_t                         decoded_done = {};

    REQUIRE(utp_frame_path_encode(path_bytes.data(), path_bytes.size(), UTP_FRAME_TYPE_PATH_CHALLENGE, &path) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_path_decode(&decoded_path, path_bytes.data(), path_bytes.size(), UTP_FRAME_TYPE_PATH_CHALLENGE) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(decoded_path.data, path.data, sizeof(path.data)) == 0);
    REQUIRE(utp_frame_handshake_done_encode(done_bytes.data(), done_bytes.size(), &done) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_handshake_done_decode(&decoded_done, done_bytes.data(), done_bytes.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_done.ack_handshake_packet_number == done.ack_handshake_packet_number);
    REQUIRE(utp_frame_path_decode(&decoded_path, path_bytes.data(), path_bytes.size(), UTP_FRAME_TYPE_PATH_RESPONSE) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_frame_handshake_done_decode(&decoded_done, done_bytes.data(), done_bytes.size() - 1u) ==
            UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("stream frame preserves its zero copy payload view", "[frame]")
{
    const std::array<uint8_t, 3>  data     = {0xaau, 0xbbu, 0xccu};
    const utp_frame_stream_t      stream   = {UTP_STREAM_FLAG_FIN, UINT32_C(0x01020304), UINT64_C(0x05060708090a0b0c),
                                              data.data(), static_cast<uint16_t>(data.size())};
    const std::array<uint8_t, 19> expected = {
        UTP_FRAME_TYPE_STREAM,
        UTP_STREAM_FLAG_FIN,
        0u,
        3u,
        1u,
        2u,
        3u,
        4u,
        5u,
        6u,
        7u,
        8u,
        9u,
        10u,
        11u,
        12u,
        0xaau,
        0xbbu,
        0xccu,
    };
    std::array<uint8_t, expected.size()> encoded = {};
    utp_frame_stream_t                   decoded = {};

    REQUIRE(utp_frame_stream_encode(encoded.data(), encoded.size(), &stream) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);
    REQUIRE(utp_frame_stream_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.flags == UTP_STREAM_FLAG_FIN);
    REQUIRE(decoded.stream_id == stream.stream_id);
    REQUIRE(decoded.offset == stream.offset);
    REQUIRE(decoded.data_length == data.size());
    REQUIRE(decoded.data == encoded.data() + 16u);
    REQUIRE(std::memcmp(decoded.data, data.data(), data.size()) == 0);
    REQUIRE(utp_frame_stream_decode(&decoded, encoded.data(), encoded.size() - 1u) == UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("connection close frame preserves a binary reason view", "[frame]")
{
    const std::array<uint8_t, 3>       reason = {0x00u, 0xaau, 0xbbu};
    const utp_frame_connection_close_t close  = {UINT16_C(0x0102), reason.data(), static_cast<uint16_t>(reason.size())};
    const std::array<uint8_t, 8>       expected = {
        UTP_FRAME_TYPE_CONNECTION_CLOSE, 1u, 2u, 0u, 3u, 0u, 0xaau, 0xbbu,
    };
    std::array<uint8_t, expected.size()> encoded = {};
    utp_frame_connection_close_t         decoded = {};

    REQUIRE(utp_frame_connection_close_encode(encoded.data(), encoded.size(), &close) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);
    REQUIRE(utp_frame_connection_close_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.error_code == close.error_code);
    REQUIRE(decoded.reason_length == reason.size());
    REQUIRE(decoded.reason == encoded.data() + 5u);
    REQUIRE(std::memcmp(decoded.reason, reason.data(), reason.size()) == 0);
}

TEST_CASE("padding frame emits only zero padding bytes", "[frame]")
{
    std::array<uint8_t, 6> encoded        = {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu};
    uint16_t               padding_length = 0u;

    REQUIRE(utp_frame_padding_encode(encoded.data(), encoded.size(), 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(encoded[0] == UTP_FRAME_TYPE_PADDING);
    REQUIRE(encoded[1] == 0u);
    REQUIRE(encoded[2] == 3u);
    REQUIRE(encoded[3] == 0u);
    REQUIRE(encoded[4] == 0u);
    REQUIRE(encoded[5] == 0u);
    REQUIRE(utp_frame_padding_decode(&padding_length, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(padding_length == 3u);
    encoded[3] = 1u;
    REQUIRE(utp_frame_padding_decode(&padding_length, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(padding_length == 3u);
}

TEST_CASE("reset stream frame round trips its terminal state", "[frame]")
{
    const utp_frame_reset_stream_t reset    = {UINT16_C(0x1122), UINT32_C(0x33445566), UINT64_C(0x778899aabbccddee)};
    const std::array<uint8_t, 15>  expected = {
        UTP_FRAME_TYPE_RESET_STREAM,
        0x11u,
        0x22u,
        0x33u,
        0x44u,
        0x55u,
        0x66u,
        0x77u,
        0x88u,
        0x99u,
        0xaau,
        0xbbu,
        0xccu,
        0xddu,
        0xeeu,
    };
    std::array<uint8_t, expected.size()> encoded = {};
    utp_frame_reset_stream_t             decoded = {};

    REQUIRE(utp_frame_reset_stream_encode(encoded.data(), encoded.size(), &reset) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);
    REQUIRE(utp_frame_reset_stream_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.error_code == reset.error_code);
    REQUIRE(decoded.stream_id == reset.stream_id);
    REQUIRE(decoded.final_size == reset.final_size);
    REQUIRE(utp_frame_reset_stream_decode(&decoded, encoded.data(), encoded.size() - 1u) ==
            UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("stop sending frame round trips its cancellation request", "[frame]")
{
    const utp_frame_stop_sending_t stop     = {UINT16_C(0x1122), UINT32_C(0x33445566)};
    const std::array<uint8_t, 7>   expected = {UTP_FRAME_TYPE_STOP_SENDING, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u};
    std::array<uint8_t, expected.size()> encoded = {};
    utp_frame_stop_sending_t             decoded = {};

    REQUIRE(utp_frame_stop_sending_encode(encoded.data(), encoded.size(), &stop) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(encoded.data(), expected.data(), expected.size()) == 0);
    REQUIRE(utp_frame_stop_sending_decode(&decoded, encoded.data(), encoded.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.error_code == stop.error_code);
    REQUIRE(decoded.stream_id == stop.stream_id);
    REQUIRE(utp_frame_stop_sending_decode(&decoded, encoded.data(), encoded.size() - 1u) ==
            UTP_INTERNAL_ERROR_OVERFLOW);
}

TEST_CASE("flow-control frames round trip their limits", "[frame]")
{
    std::array<uint8_t, 13>         encoded                 = {};
    utp_frame_max_data_t            max_data                = {UINT64_C(0x0102030405060708)};
    utp_frame_max_data_t            decoded_max_data        = {};
    utp_frame_data_blocked_t        data_blocked            = {UINT64_C(0x1112131415161718)};
    utp_frame_data_blocked_t        decoded_data_blocked    = {};
    utp_frame_max_stream_data_t     max_stream_data         = {UINT32_C(0x21222324), UINT64_C(0x25262728292a2b2c)};
    utp_frame_max_stream_data_t     decoded_max_stream_data = {};
    utp_frame_stream_data_blocked_t stream_blocked          = {UINT32_C(0x31323334), UINT64_C(0x35363738393a3b3c)};
    utp_frame_stream_data_blocked_t decoded_stream_blocked  = {};

    REQUIRE(utp_frame_max_data_encode(encoded.data(), encoded.size(), &max_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_max_data_decode(&decoded_max_data, encoded.data(), 9u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_max_data.maximum_data == max_data.maximum_data);

    REQUIRE(utp_frame_data_blocked_encode(encoded.data(), encoded.size(), &data_blocked) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_data_blocked_decode(&decoded_data_blocked, encoded.data(), 9u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_data_blocked.data_limit == data_blocked.data_limit);

    REQUIRE(utp_frame_max_stream_data_encode(encoded.data(), encoded.size(), &max_stream_data) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_max_stream_data_decode(&decoded_max_stream_data, encoded.data(), encoded.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_max_stream_data.stream_id == max_stream_data.stream_id);
    REQUIRE(decoded_max_stream_data.maximum_stream_data == max_stream_data.maximum_stream_data);

    REQUIRE(utp_frame_stream_data_blocked_encode(encoded.data(), encoded.size(), &stream_blocked) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_stream_data_blocked_decode(&decoded_stream_blocked, encoded.data(), encoded.size()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_stream_blocked.stream_id == stream_blocked.stream_id);
    REQUIRE(decoded_stream_blocked.stream_data_limit == stream_blocked.stream_data_limit);
}
