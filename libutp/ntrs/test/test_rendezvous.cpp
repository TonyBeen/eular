#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

extern "C" {
#include <utp/nat.h>

#include "proto/frame.h"
#include "proto/proto.h"
#include "rendezvous/rendezvous.h"
}

namespace {

static pid_t g_ntrs_pid = -1;

static void  fail(const char* expression, const char* file, int line)
{
    fprintf(stderr, "%s:%d: test assertion failed: %s\n", file, line, expression);
    if (g_ntrs_pid > 0) {
        (void)kill(g_ntrs_pid, SIGTERM);
        (void)waitpid(g_ntrs_pid, NULL, 0);
        g_ntrs_pid = -1;
    }
    _exit(1);
}

#define CHECK(expression)                                         \
    do {                                                          \
        if (!(expression)) fail(#expression, __FILE__, __LINE__); \
    } while (false)

struct Datagram {
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> data;
    size_t                                    length;
};

struct RendezvousFrame {
    uint8_t              type;
    std::vector<uint8_t> body;
};

static uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static sockaddr_in loopback_endpoint(uint16_t port)
{
    sockaddr_in endpoint = {};

    endpoint.sin_family      = AF_INET;
    endpoint.sin_port        = htons(port);
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return endpoint;
}

static int open_socket(uint16_t port)
{
    const int         socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    const sockaddr_in endpoint  = loopback_endpoint(port);

    CHECK(socket_fd >= 0);
    CHECK(bind(socket_fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    return socket_fd;
}

static uint16_t socket_port(int socket_fd)
{
    sockaddr_in endpoint = {};
    socklen_t   length   = sizeof(endpoint);

    CHECK(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&endpoint), &length) == 0);
    CHECK(endpoint.sin_family == AF_INET);
    return ntohs(endpoint.sin_port);
}

static bool port_available(uint16_t port)
{
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    bool      available = false;

    if (socket_fd < 0) return false;
    const sockaddr_in endpoint = loopback_endpoint(port);
    available                  = bind(socket_fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0;
    (void)close(socket_fd);
    return available;
}

static uint16_t find_service_port()
{
    const uint16_t first_port = static_cast<uint16_t>(30000u + (static_cast<unsigned>(getpid()) % 20000u));

    for (uint32_t offset = 0u; offset < 10000u; ++offset) {
        const uint32_t candidate = static_cast<uint32_t>(first_port) + offset;

        if (candidate > 65000u) break;
        if (port_available(static_cast<uint16_t>(candidate)) && port_available(static_cast<uint16_t>(candidate + 1u)) &&
            port_available(static_cast<uint16_t>(candidate + 2u)))
            return static_cast<uint16_t>(candidate);
    }
    fail("available NTRS UDP port range", __FILE__, __LINE__);
    return 0u;
}

struct PortSequence {
    int      primary;
    int      first_calibration;
    int      second_calibration;
    uint16_t primary_port;
};

static PortSequence open_port_sequence()
{
    const uint16_t first_port = static_cast<uint16_t>(40000u + (static_cast<unsigned>(getpid()) % 10000u));

    for (uint32_t offset = 0u; offset < 10000u; ++offset) {
        const uint32_t    candidate = static_cast<uint32_t>(first_port) + offset;
        int               primary;
        int               first_calibration;
        int               second_calibration;
        const sockaddr_in primary_endpoint = loopback_endpoint(static_cast<uint16_t>(candidate));
        const sockaddr_in first_endpoint   = loopback_endpoint(static_cast<uint16_t>(candidate + 10u));
        const sockaddr_in second_endpoint  = loopback_endpoint(static_cast<uint16_t>(candidate + 20u));

        if (candidate > 64000u) break;
        primary            = socket(AF_INET, SOCK_DGRAM, 0);
        first_calibration  = socket(AF_INET, SOCK_DGRAM, 0);
        second_calibration = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(first_calibration >= 0 && second_calibration >= 0);
        CHECK(primary >= 0);
        if (bind(primary, reinterpret_cast<const sockaddr*>(&primary_endpoint), sizeof(primary_endpoint)) == 0 &&
            bind(first_calibration, reinterpret_cast<const sockaddr*>(&first_endpoint), sizeof(first_endpoint)) == 0 &&
            bind(second_calibration, reinterpret_cast<const sockaddr*>(&second_endpoint), sizeof(second_endpoint)) ==
                0) {
            PortSequence sequence = {primary, first_calibration, second_calibration, static_cast<uint16_t>(candidate)};
            return sequence;
        }
        (void)close(primary);
        (void)close(first_calibration);
        (void)close(second_calibration);
    }
    fail("available UDP source port sequence", __FILE__, __LINE__);
    return PortSequence();
}

static void close_sequence(PortSequence* sequence)
{
    if (sequence->primary >= 0) (void)close(sequence->primary);
    if (sequence->first_calibration >= 0) (void)close(sequence->first_calibration);
    if (sequence->second_calibration >= 0) (void)close(sequence->second_calibration);
    sequence->primary            = -1;
    sequence->first_calibration  = -1;
    sequence->second_calibration = -1;
}

static utp_address_t loopback_address(uint16_t port)
{
    utp_address_t address = {};

    address.family     = UTP_ADDRESS_FAMILY_IPV4;
    address.port       = port;
    address.address[0] = 127u;
    address.address[3] = 1u;
    return address;
}

static bool encode_packet(uint8_t packet_type, uint32_t scid, uint64_t packet_number,
                          const std::vector<RendezvousFrame>& frames, Datagram* packet)
{
    utp_packet_header_t header         = {};
    size_t              payload_length = 0u;
    size_t              offset         = UTP_PACKET_HEADER_SIZE;

    for (size_t index = 0u; index < frames.size(); ++index) {
        if (frames[index].body.size() > UINT16_MAX ||
            payload_length > UINT16_MAX - UTP_FRAME_RENDEZVOUS_HEADER_SIZE - frames[index].body.size())
            return false;
        payload_length += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body.size();
    }
    if (UTP_PACKET_HEADER_SIZE + payload_length > packet->data.size()) return false;
    header.scid           = scid;
    header.packet_number  = packet_number;
    header.payload_length = static_cast<uint16_t>(payload_length);
    header.type           = packet_type;
    if (utp_proto_encode_header(packet->data.data(), packet->data.size(), &header) != UTP_INTERNAL_ERROR_OK)
        return false;
    for (size_t index = 0u; index < frames.size(); ++index) {
        const utp_frame_rendezvous_t frame = {frames[index].body.data(),
                                              static_cast<uint16_t>(frames[index].body.size()), frames[index].type};
        if (utp_frame_rendezvous_encode(packet->data.data() + offset, packet->data.size() - offset, &frame) !=
            UTP_INTERNAL_ERROR_OK)
            return false;
        offset += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body.size();
    }
    packet->length = offset;
    return true;
}

static void send_packet(int socket_fd, uint16_t server_port, const Datagram& packet)
{
    const sockaddr_in endpoint = loopback_endpoint(server_port);

    CHECK(sendto(socket_fd, packet.data.data(), packet.length, 0, reinterpret_cast<const sockaddr*>(&endpoint),
                 sizeof(endpoint)) == static_cast<ssize_t>(packet.length));
}

static bool receive_datagram(int socket_fd, uint32_t timeout_ms, Datagram* packet)
{
    pollfd    descriptor = {socket_fd, POLLIN, 0};
    const int ready      = poll(&descriptor, 1u, static_cast<int>(timeout_ms));

    if (ready <= 0) return false;
    packet->length = static_cast<size_t>(recv(socket_fd, packet->data.data(), packet->data.size(), 0));
    return packet->length != static_cast<size_t>(-1);
}

static bool decode_frames(const Datagram& packet, uint64_t* packet_number, std::vector<RendezvousFrame>* frames)
{
    utp_packet_view_t view   = {};
    size_t            offset = 0u;

    if (utp_packet_view_decode(&view, packet.data.data(), packet.length) != UTP_INTERNAL_ERROR_OK ||
        view.header.type != UTP_PACKET_TYPE_RENDEZVOUS || view.header.scid != 0u || view.header.dcid != 0u)
        return false;
    *packet_number = view.header.packet_number;
    frames->clear();
    while (offset < view.payload_length) {
        const uint8_t*         frame_data   = NULL;
        size_t                 frame_length = 0u;
        uint8_t                frame_type   = 0u;
        utp_frame_rendezvous_t rendezvous   = {};

        if (utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length) !=
                UTP_INTERNAL_ERROR_OK ||
            frame_type != UTP_FRAME_TYPE_RENDEZVOUS ||
            utp_frame_rendezvous_decode(&rendezvous, frame_data, frame_length) != UTP_INTERNAL_ERROR_OK)
            return false;
        RendezvousFrame decoded = {
            rendezvous.message_type,
            std::vector<uint8_t>(rendezvous.payload, rendezvous.payload + rendezvous.payload_length)};
        frames->push_back(decoded);
    }
    return !frames->empty();
}

static bool receive_message(int socket_fd, uint8_t message_type, uint32_t timeout_ms, uint64_t* packet_number,
                            std::vector<uint8_t>* body)
{
    const uint64_t deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        Datagram                     packet = {};
        std::vector<RendezvousFrame> frames;
        uint64_t                     received_packet_number = 0u;
        const uint64_t               remaining              = deadline - now_ms();

        if (!receive_datagram(socket_fd, static_cast<uint32_t>(remaining > 100u ? 100u : remaining), &packet)) continue;
        if (!decode_frames(packet, &received_packet_number, &frames)) continue;
        for (size_t index = 0u; index < frames.size(); ++index) {
            if (frames[index].type != message_type) continue;
            *packet_number = received_packet_number;
            *body          = frames[index].body;
            return true;
        }
    }
    return false;
}

static RendezvousFrame encode_register_frame(const char* peer_id, uint8_t nat_class, uint16_t local_port,
                                             uint64_t request_id)
{
    utp_rendezvous_register_t registration = {};
    const utp_address_t       address      = loopback_address(local_port);
    std::vector<uint8_t>      body(UTP_PACKET_MTU_FLOOR);
    size_t                    body_length = 0u;

    registration.peer_id                  = reinterpret_cast<const uint8_t*>(peer_id);
    registration.local_candidates         = &address;
    registration.reported_public_endpoint = &address;
    registration.registration_request_id  = request_id;
    registration.local_port               = local_port;
    registration.peer_id_length           = static_cast<uint8_t>(strlen(peer_id));
    registration.nat_class                = nat_class;
    registration.local_family             = UTP_ADDRESS_FAMILY_IPV4;
    registration.local_candidate_count    = 1u;
    CHECK(utp_rendezvous_register_encode(body.data(), body.size(), &registration, &body_length) ==
          UTP_INTERNAL_ERROR_OK);
    body.resize(body_length);
    return RendezvousFrame{UTP_RENDEZVOUS_MESSAGE_REGISTER, body};
}

static utp_rendezvous_registered_t register_peer(int socket_fd, uint16_t server_port, const char* peer_id,
                                                 uint8_t nat_class, uint64_t request_id)
{
    const RendezvousFrame       frame = encode_register_frame(peer_id, nat_class, socket_port(socket_fd), request_id);
    utp_rendezvous_registered_t registered = {};

    for (uint64_t attempt = 1u; attempt <= 3u; ++attempt) {
        const std::vector<RendezvousFrame> frames(1u, frame);
        std::vector<uint8_t>               body;
        uint64_t                           packet_number = 0u;
        Datagram                           packet        = {};

        CHECK(encode_packet(UTP_PACKET_TYPE_RENDEZVOUS, 0u, attempt, frames, &packet));
        send_packet(socket_fd, server_port, packet);
        if (!receive_message(socket_fd, UTP_RENDEZVOUS_MESSAGE_REGISTERED, 500u, &packet_number, &body)) continue;
        CHECK(packet_number != 0u);
        CHECK(utp_rendezvous_registered_decode(&registered, body.data(), body.size()) == UTP_INTERNAL_ERROR_OK);
        CHECK(registered.registration_request_id == request_id);
        return registered;
    }
    fail("NTRS REGISTERED response", __FILE__, __LINE__);
    return registered;
}

static void send_ping(int socket_fd, uint16_t server_port, const uint8_t token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE],
                      uint64_t calibration_id, uint64_t packet_number)
{
    utp_rendezvous_ping_t        ping = {};
    std::vector<uint8_t>         body(UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t));
    std::vector<RendezvousFrame> frames;
    Datagram                     packet = {};

    memcpy(ping.registration_token, token, sizeof(ping.registration_token));
    ping.calibration_id = calibration_id;
    CHECK(utp_rendezvous_ping_encode(body.data(), body.size(), &ping) == UTP_INTERNAL_ERROR_OK);
    frames.push_back(RendezvousFrame{UTP_RENDEZVOUS_MESSAGE_PING, body});
    CHECK(encode_packet(UTP_PACKET_TYPE_RENDEZVOUS, 0u, packet_number, frames, &packet));
    send_packet(socket_fd, server_port, packet);
}

static void expect_pong(int socket_fd, const uint8_t token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE],
                        uint64_t packet_number)
{
    std::vector<uint8_t>  body;
    uint64_t              response_packet_number = 0u;
    utp_rendezvous_pong_t pong                   = {};

    CHECK(receive_message(socket_fd, UTP_RENDEZVOUS_MESSAGE_PONG, 1000u, &response_packet_number, &body));
    CHECK(response_packet_number != 0u);
    CHECK(utp_rendezvous_pong_decode(&pong, body.data(), body.size()) == UTP_INTERNAL_ERROR_OK);
    CHECK(memcmp(pong.registration_token, token, sizeof(pong.registration_token)) == 0);
    CHECK(pong.acknowledged_packet_number == packet_number);
}

static Datagram make_request(const char* source_peer_id, const char* target_peer_id, uint8_t source_nat_class,
                             uint16_t source_port, const uint8_t rendezvous_id[UTP_RENDEZVOUS_ID_SIZE])
{
    utp_rendezvous_request_t     request = {};
    const utp_address_t          address = loopback_address(source_port);
    std::vector<uint8_t>         body(UTP_PACKET_MTU_FLOOR);
    std::vector<RendezvousFrame> frames;
    size_t                       body_length = 0u;
    Datagram                     packet      = {};

    request.source_peer_id           = reinterpret_cast<const uint8_t*>(source_peer_id);
    request.target_peer_id           = reinterpret_cast<const uint8_t*>(target_peer_id);
    request.local_candidates         = &address;
    request.reported_public_endpoint = &address;
    request.local_port               = source_port;
    request.source_peer_id_length    = static_cast<uint8_t>(strlen(source_peer_id));
    request.target_peer_id_length    = static_cast<uint8_t>(strlen(target_peer_id));
    request.source_nat_class         = source_nat_class;
    request.local_family             = UTP_ADDRESS_FAMILY_IPV4;
    request.local_candidate_count    = 1u;
    memcpy(request.rendezvous_id, rendezvous_id, sizeof(request.rendezvous_id));
    CHECK(utp_rendezvous_request_encode(body.data(), body.size(), &request, &body_length) == UTP_INTERNAL_ERROR_OK);
    body.resize(body_length);
    frames.push_back(RendezvousFrame{UTP_RENDEZVOUS_MESSAGE_REQUEST, body});
    CHECK(encode_packet(UTP_PACKET_TYPE_INITIAL, 1u, 1u, frames, &packet));
    return packet;
}

static utp_rendezvous_redirect_t receive_redirect(int socket_fd)
{
    std::vector<uint8_t>      body;
    uint64_t                  packet_number = 0u;
    utp_rendezvous_redirect_t redirect      = {};

    CHECK(receive_message(socket_fd, UTP_RENDEZVOUS_MESSAGE_REDIRECT, 1500u, &packet_number, &body));
    CHECK(packet_number != 0u);
    CHECK(utp_rendezvous_redirect_decode(&redirect, body.data(), body.size()) == UTP_INTERNAL_ERROR_OK);
    return redirect;
}

static utp_rendezvous_calibrate_t receive_calibrate(int socket_fd)
{
    std::vector<uint8_t>       body;
    uint64_t                   packet_number = 0u;
    utp_rendezvous_calibrate_t calibrate     = {};

    CHECK(receive_message(socket_fd, UTP_RENDEZVOUS_MESSAGE_CALIBRATE, 1000u, &packet_number, &body));
    CHECK(packet_number != 0u);
    CHECK(utp_rendezvous_calibrate_decode(&calibrate, body.data(), body.size()) == UTP_INTERNAL_ERROR_OK);
    return calibrate;
}

static void receive_forward_and_pong(int target_socket, uint16_t server_port,
                                     const uint8_t registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE],
                                     const uint8_t expected_rendezvous_id[UTP_RENDEZVOUS_ID_SIZE],
                                     utp_rendezvous_forward_t* forward)
{
    Datagram                     packet = {};
    std::vector<RendezvousFrame> frames;
    uint64_t                     packet_number = 0u;
    utp_rendezvous_ping_t        ping          = {};
    utp_rendezvous_pong_t        pong          = {};
    std::vector<uint8_t>         pong_body(UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t));
    std::vector<RendezvousFrame> pong_frames;
    Datagram                     pong_packet = {};

    CHECK(receive_datagram(target_socket, 1500u, &packet));
    CHECK(decode_frames(packet, &packet_number, &frames));
    CHECK(frames.size() == 2u);
    CHECK(frames[0u].type == UTP_RENDEZVOUS_MESSAGE_PING);
    CHECK(frames[1u].type == UTP_RENDEZVOUS_MESSAGE_FORWARD);
    CHECK(utp_rendezvous_ping_decode(&ping, frames[0u].body.data(), frames[0u].body.size()) == UTP_INTERNAL_ERROR_OK);
    CHECK(memcmp(ping.registration_token, registration_token, sizeof(ping.registration_token)) == 0);
    CHECK(utp_rendezvous_forward_decode(forward, frames[1u].body.data(), frames[1u].body.size()) ==
          UTP_INTERNAL_ERROR_OK);
    CHECK(memcmp(forward->rendezvous_id, expected_rendezvous_id, sizeof(forward->rendezvous_id)) == 0);
    memcpy(pong.registration_token, registration_token, sizeof(pong.registration_token));
    pong.acknowledged_packet_number = packet_number;
    CHECK(utp_rendezvous_pong_encode(pong_body.data(), pong_body.size(), &pong) == UTP_INTERNAL_ERROR_OK);
    pong_frames.push_back(RendezvousFrame{UTP_RENDEZVOUS_MESSAGE_PONG, pong_body});
    CHECK(encode_packet(UTP_PACKET_TYPE_RENDEZVOUS, 0u, 2u, pong_frames, &pong_packet));
    send_packet(target_socket, server_port, pong_packet);
}

class NtrsProcess
{
public:
    NtrsProcess(const char* executable, uint16_t service_port) : pid_(-1)
    {
        const std::string port            = std::to_string(service_port);
        const std::string calibration_one = std::to_string(static_cast<uint16_t>(service_port + 1u));
        const std::string calibration_two = std::to_string(static_cast<uint16_t>(service_port + 2u));

        pid_ = fork();
        CHECK(pid_ >= 0);
        if (pid_ == 0) {
            execl(executable, executable, "-a", "127.0.0.1", "-e", "127.0.0.1", "-p", port.c_str(), "-c",
                  calibration_one.c_str(), "-c", calibration_two.c_str(), static_cast<char*>(NULL));
            _exit(127);
        }
        g_ntrs_pid = pid_;
    }

    ~NtrsProcess()
    {
        if (pid_ > 0) {
            (void)kill(pid_, SIGTERM);
            (void)waitpid(pid_, NULL, 0);
        }
        if (g_ntrs_pid == pid_) g_ntrs_pid = -1;
    }

private:
    pid_t pid_;
};

static void test_registered_symmetric_prediction(uint16_t service_port)
{
    PortSequence                target                                = open_port_sequence();
    const int                   source                                = open_socket(0u);
    const uint8_t               rendezvous_id[UTP_RENDEZVOUS_ID_SIZE] = {1u};
    utp_rendezvous_registered_t registered;
    utp_rendezvous_redirect_t   redirect;
    utp_rendezvous_forward_t    forward = {};
    Datagram                    request;

    registered = register_peer(target.primary, service_port, "symmetric-target", UTP_NAT_CLASS_SYMMETRIC, 1u);
    CHECK(registered.calibration_id != 0u);
    CHECK(registered.calibration_endpoint_count == 2u);
    send_ping(target.first_calibration, registered.calibration_endpoints[0u].port, registered.registration_token,
              registered.calibration_id, 1u);
    expect_pong(target.first_calibration, registered.registration_token, 1u);
    send_ping(target.second_calibration, registered.calibration_endpoints[1u].port, registered.registration_token,
              registered.calibration_id, 1u);
    expect_pong(target.second_calibration, registered.registration_token, 1u);

    request = make_request("source-port-restricted", "symmetric-target", UTP_NAT_CLASS_PORT_RESTRICTED,
                           socket_port(source), rendezvous_id);
    send_packet(source, service_port, request);
    redirect = receive_redirect(source);
    CHECK(redirect.target_plan.public_candidate_count == UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES);
    CHECK(redirect.target_plan.public_candidates[0u].port == target.primary_port);
    CHECK(redirect.target_plan.public_candidates[1u].port == static_cast<uint16_t>(target.primary_port + 30u));
    receive_forward_and_pong(target.primary, service_port, registered.registration_token, rendezvous_id, &forward);
    CHECK(forward.source_plan.public_candidates[0u].port == socket_port(source));

    send_packet(source, service_port, request);
    redirect = receive_redirect(source);
    CHECK(redirect.target_plan.public_candidates[1u].port == static_cast<uint16_t>(target.primary_port + 30u));
    {
        Datagram ignored = {};
        CHECK(!receive_datagram(target.primary, 300u, &ignored));
    }
    (void)close(source);
    close_sequence(&target);
}

static void test_temporary_symmetric_calibration(uint16_t service_port)
{
    const int                   target                                = open_socket(0u);
    PortSequence                source                                = open_port_sequence();
    const uint8_t               rendezvous_id[UTP_RENDEZVOUS_ID_SIZE] = {2u};
    utp_rendezvous_registered_t registered;
    utp_rendezvous_calibrate_t  calibrate;
    utp_rendezvous_redirect_t   redirect;
    utp_rendezvous_forward_t    forward = {};
    Datagram                    request;

    registered = register_peer(target, service_port, "port-restricted-target", UTP_NAT_CLASS_PORT_RESTRICTED, 2u);
    request    = make_request("temporary-symmetric", "port-restricted-target", UTP_NAT_CLASS_SYMMETRIC,
                              source.primary_port, rendezvous_id);
    send_packet(source.primary, service_port, request);
    calibrate = receive_calibrate(source.primary);
    CHECK(memcmp(calibrate.rendezvous_id, rendezvous_id, sizeof(calibrate.rendezvous_id)) == 0);
    CHECK(calibrate.endpoint_count == 2u);
    send_ping(source.first_calibration, calibrate.endpoints[0u].port, calibrate.calibration_token,
              calibrate.calibration_id, 1u);
    expect_pong(source.first_calibration, calibrate.calibration_token, 1u);
    send_ping(source.second_calibration, calibrate.endpoints[1u].port, calibrate.calibration_token,
              calibrate.calibration_id, 1u);
    expect_pong(source.second_calibration, calibrate.calibration_token, 1u);
    redirect = receive_redirect(source.primary);
    CHECK(redirect.target_plan.public_candidates[0u].port == socket_port(target));
    receive_forward_and_pong(target, service_port, registered.registration_token, rendezvous_id, &forward);
    CHECK(forward.source_plan.public_candidate_count == UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES);
    CHECK(forward.source_plan.public_candidates[0u].port == source.primary_port);
    CHECK(forward.source_plan.public_candidates[1u].port == static_cast<uint16_t>(source.primary_port + 30u));
    (void)close(target);
    close_sequence(&source);
}

static void test_temporary_calibration_timeout(uint16_t service_port)
{
    const int                   target                                = open_socket(0u);
    PortSequence                source                                = open_port_sequence();
    const uint8_t               rendezvous_id[UTP_RENDEZVOUS_ID_SIZE] = {3u};
    utp_rendezvous_registered_t registered;
    utp_rendezvous_calibrate_t  calibrate;
    utp_rendezvous_redirect_t   redirect;
    utp_rendezvous_forward_t    forward = {};
    Datagram                    request;
    const uint64_t              started_at = now_ms();

    registered = register_peer(target, service_port, "timeout-target", UTP_NAT_CLASS_PORT_RESTRICTED, 3u);
    request    = make_request("timeout-symmetric", "timeout-target", UTP_NAT_CLASS_SYMMETRIC, source.primary_port,
                              rendezvous_id);
    send_packet(source.primary, service_port, request);
    calibrate = receive_calibrate(source.primary);
    CHECK(calibrate.endpoint_count == 2u);
    redirect = receive_redirect(source.primary);
    CHECK(now_ms() - started_at >= 900u);
    CHECK(redirect.target_plan.public_candidates[0u].port == socket_port(target));
    receive_forward_and_pong(target, service_port, registered.registration_token, rendezvous_id, &forward);
    CHECK(forward.source_plan.public_candidate_count == UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES);
    CHECK(forward.source_plan.public_candidates[0u].port == source.primary_port);
    (void)close(target);
    close_sequence(&source);
}

}  // namespace

int main(int argc, char** argv)
{
    const uint16_t service_port = find_service_port();

    CHECK(argc == 2);
    NtrsProcess server(argv[1], service_port);
    test_registered_symmetric_prediction(service_port);
    test_temporary_symmetric_calibration(service_port);
    test_temporary_calibration_timeout(service_port);
    return 0;
}
