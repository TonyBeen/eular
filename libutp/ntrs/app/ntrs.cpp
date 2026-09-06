#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/util.h>
#include <ntrs/service.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <utils/CLI11.hpp>
#include <utp/nat.h>

#include "app_log.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "rendezvous/rendezvous.h"
#include "service_util.h"

#define fprintf(stream, ...) UTP_NTRS_APP_LOG(stream, __VA_ARGS__)

struct Registration {
    utp_ntrs_endpoint_t                                            endpoint;
    utp_ntrs_endpoint_t                                            calibration_ip;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    token;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> local_candidates;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES>  observed_addresses;
    std::array<uint16_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES + 1u> calibration_ports;
    std::array<uint64_t, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES>       observed_at_unix_ms;
    utp_address_t                                                  reported_public_endpoint;
    uint64_t                                                       last_register_request_id;
    uint64_t                                                       last_activity_ms;
    uint64_t                                                       keepalive_packet_number;
    uint64_t                                                       last_keepalive_ping_ms;
    uint16_t                                                       local_port;
    uint8_t                                                        nat_class;
    uint8_t                                                        local_family;
    uint8_t                                                        local_candidate_count;
    uint8_t                                                        observed_address_count;
    uint8_t                                                        calibration_endpoint_count;
    uint8_t                                                        calibration_received_mask;
    bool                                                           has_reported_public_endpoint;
    bool                                                           last_register_was_initial;
    bool                                                           calibration_ip_consistent;
    uint64_t                                                       calibration_id;

    bool    matchesToken(const uint8_t value[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE]) const;
    bool    acceptsFamily(const utp_ntrs_endpoint_t& observed) const;
    void    touch(uint64_t now_ms);
    void    updateEndpoint(const utp_ntrs_endpoint_t& observed);
    void    update(const utp_rendezvous_register_t& request, const utp_ntrs_endpoint_t& observed);
    uint8_t updateObservedAddresses(const utp_rendezvous_address_update_t& update);
    void    recordCalibrationEndpoint(uint8_t index, const utp_ntrs_endpoint_t& observed);
};

struct PendingRendezvous {
    utp_ntrs_endpoint_t                                            source_endpoint;
    utp_ntrs_endpoint_t                                            target_endpoint;
    sockaddr_storage                                               source_socket_address;
    sockaddr_storage                                               target_socket_address;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    target_token;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    calibration_token;
    std::array<uint8_t, UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE>           punch_token;
    std::array<uint8_t, UTP_RENDEZVOUS_ID_SIZE>                    rendezvous_id;
    std::array<uint8_t, UTP_PEER_ID_MAX_LENGTH>                    source_peer_id;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> source_local_candidates;
    std::array<uint16_t, 3u>                                       calibration_ports;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>                      forward_packet;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>                      redirect_body;
    utp_address_t                                                  source_reported_public_endpoint;
    utp_ntrs_endpoint_t                                            calibration_ip;
    uint64_t                                                       created_at_ms;
    uint64_t                                                       calibration_id;
    uint64_t                                                       calibration_deadline_ms;
    uint64_t                                                       forward_packet_number;
    uint64_t                                                       forward_retry_at_ms;
    size_t                                                         forward_packet_length;
    size_t                                                         redirect_body_length;
    socklen_t                                                      source_socket_length;
    socklen_t                                                      target_socket_length;
    uint16_t                                                       source_local_port;
    uint8_t                                                        source_peer_id_length;
    uint8_t                                                        source_local_family;
    uint8_t                                                        source_local_candidate_count;
    uint8_t                                                        calibration_received_mask;
    uint8_t                                                        forward_retries_remaining;
    uint32_t                                                       forward_retry_delay_ms;
    bool                                                           has_source_reported_public_endpoint;
    bool                                                           calibration_ip_consistent;
    bool                                                           calibration_pending;
    bool                                                           forward_delivered;
};

struct OutgoingDatagram {
    sockaddr_storage                          peer;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> packet;
    size_t                                    packet_length;
    socklen_t                                 peer_length;
};

class NtrsServer;

struct UdpSocket {
    NtrsServer*                  server;
    event*                       read_event;
    event*                       write_event;
    std::deque<OutgoingDatagram> output_queue;
    int                          fd;
    uint8_t                      calibration_index;
    bool                         write_event_active;
};

struct OutgoingFrame;

class NtrsServer
{
public:
    NtrsServer();
    ~NtrsServer();

    bool start(const utp_ntrs_endpoint_t& bind_endpoint, const utp_ntrs_endpoint_t& advertised_endpoint,
               const char* interface_name, const std::vector<uint16_t>& calibration_ports,
               uint8_t public_candidate_count, uint32_t registration_timeout_ms, uint32_t keepalive_interval_ms);
    int  run(const utp_ntrs_endpoint_t& endpoint);

private:
    NtrsServer(const NtrsServer&)            = delete;
    NtrsServer& operator=(const NtrsServer&) = delete;

    static void OnReadEvent(evutil_socket_t, short, void* user_data);
    static void OnTimerEvent(evutil_socket_t, short, void* user_data);
    static void OnWriteEvent(evutil_socket_t, short, void* user_data);

    void        stopForUdpSendError(int error);
    void        releaseSocket(UdpSocket* socket);
    bool        createSocket(UdpSocket* socket, const utp_ntrs_endpoint_t& endpoint, const char* interface_name,
                             uint8_t calibration_index);
    bool sendDatagram(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, const uint8_t* packet,
                      size_t packet_length);
    bool sendFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, const OutgoingFrame* frames,
                    size_t frame_count, uint64_t* packet_number,
                    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>* encoded_packet        = NULL,
                    size_t*                                    encoded_packet_length = NULL);
    bool sendMessage(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, uint8_t message_type,
                     const uint8_t* body, size_t body_length);
    bool sendRejected(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                      uint8_t rejected_message_type, const uint8_t* reference_id, uint8_t reference_length,
                      uint16_t reason_code);
    bool sendKeepalivePing(Registration* registration, uint64_t now_ms);
    bool beginCalibration(Registration* registration);
    bool sendCalibration(PendingRendezvous* transaction);
    bool completePendingRendezvous(PendingRendezvous* transaction);
    void expirePendingRendezvous(uint64_t now_ms);
    void expireRegistrations(uint64_t now_ms);
    void sendKeepalivePings(uint64_t now_ms);
    void retryPendingRendezvous(uint64_t now_ms);
    void handleRegister(const sockaddr_storage& peer, socklen_t peer_length, const utp_ntrs_endpoint_t& observed,
                        const utp_frame_rendezvous_t& frame);
    void handlePing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                    const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                    const utp_frame_rendezvous_t& frame);
    void handlePong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void handleAddressUpdate(const sockaddr_storage& peer, socklen_t peer_length, const utp_ntrs_endpoint_t& observed,
                             const utp_frame_rendezvous_t& frame);
    void handleUnregister(const sockaddr_storage& peer, socklen_t peer_length, const utp_frame_rendezvous_t& frame);
    void handleRequest(const sockaddr_storage& peer, socklen_t peer_length, const utp_ntrs_endpoint_t& observed,
                       const utp_frame_rendezvous_t& frame);
    void onRead(UdpSocket* socket);
    void onTimer();
    void onWrite(UdpSocket* socket);

    event_base*                                                    base_;
    event*                                                         timer_event_;
    UdpSocket                                                      primary_socket_;
    std::array<UdpSocket, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES>     calibration_sockets_;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> calibration_endpoints_;
    uint64_t                                                       next_packet_number_;
    uint8_t                                                        calibration_endpoint_count_;
    uint8_t                                                        public_candidate_count_;
    uint32_t                                                       registration_timeout_ms_;
    uint32_t                                                       keepalive_interval_ms_;
    bool                                                           fatal_socket_error_;
    std::unordered_map<std::string, Registration>                  registrations_;
    std::unordered_map<std::string, uint64_t>                      unregistration_tombstones_;
    std::unordered_map<std::string, PendingRendezvous>             pending_rendezvous_;
};

static const uint64_t k_pending_rendezvous_lifetime_ms       = 30000u;
static const uint64_t k_unregistration_tombstone_lifetime_ms = 30000u;
static const uint32_t k_registration_timeout_default_ms      = 90000u;
static const uint32_t k_keepalive_interval_default_ms        = 30000u;
static const uint32_t k_forward_retry_initial_delay_ms       = 1000u;
static const uint32_t k_forward_retry_max_delay_ms           = 8000u;
static const uint32_t k_forward_retry_send_failure_delay_ms  = 100u;
static const uint8_t  k_forward_retry_count                  = 3u;
static const uint32_t k_temporary_calibration_timeout_ms     = 1000u;
static const uint8_t  k_temporary_calibration_endpoint_count = 2u;
static const size_t   k_output_queue_capacity                = 128u;
static const uint8_t  k_primary_socket_index                 = UINT8_MAX;

enum NtrsRejectionReason {
    k_rejection_peer_not_found = 2u,
    k_rejection_peer_id_exists = 3u,
    k_rejection_token_invalid  = 4u
};

static bool is_fatal_udp_send_error(int error)
{
#ifdef EBADF
    if (error == EBADF) return true;
#endif
#ifdef ENOTSOCK
    if (error == ENOTSOCK) return true;
#endif
#ifdef EINVAL
    if (error == EINVAL) return true;
#endif
#ifdef ENOBUFS
    if (error == ENOBUFS) return true;
#endif
    return false;
}

NtrsServer::NtrsServer()
    : base_(NULL),
      timer_event_(NULL),
      primary_socket_(),
      calibration_sockets_(),
      calibration_endpoints_(),
      next_packet_number_(1u),
      calibration_endpoint_count_(0u),
      public_candidate_count_(UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES),
      registration_timeout_ms_(k_registration_timeout_default_ms),
      keepalive_interval_ms_(k_keepalive_interval_default_ms),
      fatal_socket_error_(false)
{
    primary_socket_.fd = -1;
    for (size_t index = 0u; index < calibration_sockets_.size(); ++index) calibration_sockets_[index].fd = -1;
}

NtrsServer::~NtrsServer()
{
    for (size_t index = 0u; index < calibration_sockets_.size(); ++index) releaseSocket(&calibration_sockets_[index]);
    releaseSocket(&primary_socket_);
    if (timer_event_ != NULL) event_free(timer_event_);
    if (base_ != NULL) event_base_free(base_);
}

void NtrsServer::stopForUdpSendError(int error)
{
    if (fatal_socket_error_) return;
    fatal_socket_error_ = true;
    fprintf(stderr, "NTRS [Fatal] udp send errno=%d\n", error);
    if (base_ != NULL) event_base_loopbreak(base_);
}

void NtrsServer::releaseSocket(UdpSocket* socket)
{
    if (socket == NULL) return;
    if (socket->write_event != NULL) event_free(socket->write_event);
    if (socket->read_event != NULL) event_free(socket->read_event);
    if (socket->fd >= 0) close(socket->fd);
    *socket    = {};
    socket->fd = -1;
}

bool NtrsServer::createSocket(UdpSocket* socket, const utp_ntrs_endpoint_t& endpoint, const char* interface_name,
                              uint8_t calibration_index)
{
    sockaddr_storage socket_address = {};
    socklen_t        socket_length  = 0u;

    if (socket == NULL || !utp_ntrs_endpoint_to_sockaddr(&endpoint, &socket_address, &socket_length)) return false;
    *socket                   = {};
    socket->fd                = -1;
    socket->server            = this;
    socket->calibration_index = calibration_index;
    if ((socket->fd = ::socket(endpoint.family, SOCK_DGRAM, 0)) < 0 ||
        !utp_ntrs_socket_bind_interface(socket->fd, interface_name) ||
        bind(socket->fd, reinterpret_cast<const sockaddr*>(&socket_address), socket_length) != 0 ||
        evutil_make_socket_nonblocking(socket->fd) != 0 ||
        (socket->read_event = event_new(base_, socket->fd, EV_READ | EV_PERSIST, OnReadEvent, socket)) == NULL ||
        (socket->write_event = event_new(base_, socket->fd, EV_WRITE | EV_PERSIST, OnWriteEvent, socket)) == NULL ||
        event_add(socket->read_event, NULL) != 0) {
        releaseSocket(socket);
        return false;
    }
    return true;
}

static bool token_is_zero(const uint8_t* token)
{
    uint8_t value = 0u;
    for (size_t index = 0u; index < UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE; ++index) value |= token[index];
    return value == 0u;
}

static bool random_token(std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>* token)
{
    size_t offset = 0u;
    while (offset < token->size()) {
        const ssize_t count = ::getrandom(token->data() + offset, token->size() - offset, 0);
        if (count > 0)
            offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return !token_is_zero(token->data());
}

static bool random_u64(uint64_t* value)
{
    std::array<uint8_t, sizeof(*value)> bytes = {};

    if (value == NULL || !random_token(&bytes)) return false;
    memcpy(value, bytes.data(), sizeof(*value));
    return *value != 0u;
}

static bool random_port(uint16_t* port)
{
    uint16_t value = 0u;
    ssize_t  count;

    if (port == NULL) return false;
    do {
        do {
            count = ::getrandom(&value, sizeof(value), 0);
        } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(sizeof(value))) return false;
    } while (value == 0u);
    *port = value;
    return true;
}

static bool endpoint_to_address(const utp_ntrs_endpoint_t& endpoint, utp_address_t* address)
{
    size_t address_length = 0u;

    if (endpoint.family == AF_INET) {
        address->family = UTP_ADDRESS_FAMILY_IPV4;
        address_length  = 4u;
    } else if (endpoint.family == AF_INET6) {
        address->family = UTP_ADDRESS_FAMILY_IPV6;
        address_length  = 16u;
    } else {
        return false;
    }
    address->port = endpoint.port;
    memcpy(address->address, endpoint.address, address_length);
    return endpoint.port != 0u;
}

static bool endpoint_is_unspecified(const utp_ntrs_endpoint_t& endpoint)
{
    const size_t address_length = endpoint.family == AF_INET ? 4u : endpoint.family == AF_INET6 ? 16u : 0u;

    if (address_length == 0u) return true;
    for (size_t index = 0u; index < address_length; ++index) {
        if (endpoint.address[index] != 0u) return false;
    }
    return true;
}

static bool endpoints_equal(const utp_ntrs_endpoint_t& left, const utp_ntrs_endpoint_t& right)
{
    const size_t address_length = left.family == AF_INET ? 4u : left.family == AF_INET6 ? 16u : 0u;

    return address_length != 0u && left.family == right.family && left.port == right.port &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool endpoint_ips_equal(const utp_ntrs_endpoint_t& left, const utp_ntrs_endpoint_t& right)
{
    const size_t address_length = left.family == AF_INET ? 4u : left.family == AF_INET6 ? 16u : 0u;

    return address_length != 0u && left.family == right.family &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool addresses_equal(const utp_address_t& left, const utp_address_t& right)
{
    const size_t address_length = left.family == UTP_ADDRESS_FAMILY_IPV4   ? 4u
                                  : left.family == UTP_ADDRESS_FAMILY_IPV6 ? 16u
                                                                           : 0u;

    return address_length != 0u && left.family == right.family && left.port == right.port &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool address_matches_endpoint_ip(const utp_address_t& address, const utp_ntrs_endpoint_t& endpoint)
{
    const size_t  address_length  = address.family == UTP_ADDRESS_FAMILY_IPV4   ? 4u
                                    : address.family == UTP_ADDRESS_FAMILY_IPV6 ? 16u
                                                                                : 0u;
    const uint8_t endpoint_family = endpoint.family == AF_INET    ? static_cast<uint8_t>(UTP_ADDRESS_FAMILY_IPV4)
                                    : endpoint.family == AF_INET6 ? static_cast<uint8_t>(UTP_ADDRESS_FAMILY_IPV6)
                                                                  : 0u;

    return address_length != 0u && address.family == endpoint_family &&
           memcmp(address.address, endpoint.address, address_length) == 0;
}

static bool append_public_candidate(utp_rendezvous_candidate_plan_t* plan, const utp_address_t& candidate)
{
    for (uint8_t index = 0u; index < plan->public_candidate_count; ++index) {
        if (addresses_equal(plan->decoded_public_candidates[index], candidate)) return true;
    }
    if (plan->public_candidate_count >= UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES) return false;
    plan->decoded_public_candidates[plan->public_candidate_count++] = candidate;
    return true;
}

static bool make_candidate_plan(utp_rendezvous_candidate_plan_t* plan, uint8_t family, uint16_t local_port,
                                const utp_address_t* local_candidates, uint8_t local_candidate_count,
                                const utp_address_t*       reported_public_endpoint,
                                const utp_ntrs_endpoint_t& observed_endpoint, uint8_t public_candidate_count)
{
    utp_address_t observed_public_endpoint = {};

    if (plan == NULL || public_candidate_count == 0u || public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        local_port == 0u || local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        !endpoint_to_address(observed_endpoint, &observed_public_endpoint) ||
        observed_public_endpoint.family != family || (local_candidate_count != 0u && local_candidates == NULL))
        return false;
    *plan                       = {};
    plan->family                = family;
    plan->local_port            = local_port;
    plan->local_candidates      = local_candidates;
    plan->local_candidate_count = local_candidate_count;
    if (reported_public_endpoint != NULL &&
        (reported_public_endpoint->family != family || reported_public_endpoint->port == 0u))
        return false;
    if (reported_public_endpoint != NULL && !append_public_candidate(plan, *reported_public_endpoint)) return false;
    if (plan->public_candidate_count < public_candidate_count &&
        !append_public_candidate(plan, observed_public_endpoint))
        return false;
    plan->public_candidates = plan->decoded_public_candidates;
    return plan->public_candidate_count != 0u;
}

bool Registration::matchesToken(const uint8_t value[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE]) const
{
    return memcmp(value, token.data(), token.size()) == 0;
}

bool Registration::acceptsFamily(const utp_ntrs_endpoint_t& observed) const
{
    return (local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family == AF_INET) ||
           (local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family == AF_INET6);
}

void Registration::updateEndpoint(const utp_ntrs_endpoint_t& observed)
{
    if (!endpoint_ips_equal(endpoint, observed)) {
        observed_addresses     = {};
        observed_at_unix_ms    = {};
        observed_address_count = 0u;
    }
    endpoint = observed;
}

void Registration::touch(uint64_t now_ms)
{
    last_activity_ms        = now_ms;
    keepalive_packet_number = 0u;
    last_keepalive_ping_ms  = 0u;
}

void Registration::update(const utp_rendezvous_register_t& request, const utp_ntrs_endpoint_t& observed)
{
    updateEndpoint(observed);
    nat_class                    = request.nat_class;
    local_family                 = request.local_family;
    local_port                   = request.local_port;
    local_candidate_count        = request.local_candidate_count;
    has_reported_public_endpoint = request.reported_public_endpoint != NULL;
    if (has_reported_public_endpoint) reported_public_endpoint = *request.reported_public_endpoint;
    for (uint8_t index = 0u; index < request.local_candidate_count; ++index)
        local_candidates[index] = request.local_candidates[index];
    touch(utp_ntrs_now_ms());
}

uint8_t Registration::updateObservedAddresses(const utp_rendezvous_address_update_t& update)
{
    uint8_t accepted = 0u;

    for (uint8_t index = 0u; index < update.sample_count; ++index) {
        const utp_address_t& sample      = update.samples[index];
        uint8_t              replacement = 0u;

        if (!address_matches_endpoint_ip(sample, endpoint)) continue;
        for (uint8_t existing = 0u; existing < observed_address_count; ++existing) {
            if (!addresses_equal(sample, observed_addresses[existing])) continue;
            if (update.observed_at_unix_ms[index] >= observed_at_unix_ms[existing]) {
                observed_at_unix_ms[existing] = update.observed_at_unix_ms[index];
                ++accepted;
            }
            replacement = UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES;
            break;
        }
        if (replacement == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) continue;
        if (observed_address_count < UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) {
            replacement = observed_address_count++;
        } else {
            for (uint8_t existing = 1u; existing < UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES; ++existing) {
                if (observed_at_unix_ms[existing] < observed_at_unix_ms[replacement]) replacement = existing;
            }
            if (update.observed_at_unix_ms[index] <= observed_at_unix_ms[replacement]) continue;
        }
        observed_addresses[replacement]  = sample;
        observed_at_unix_ms[replacement] = update.observed_at_unix_ms[index];
        ++accepted;
    }
    return accepted;
}

void Registration::recordCalibrationEndpoint(uint8_t index, const utp_ntrs_endpoint_t& observed)
{
    if (index >= calibration_endpoint_count || observed.port == 0u) return;
    if (!endpoint_ips_equal(calibration_ip, observed)) {
        calibration_ip_consistent = false;
        return;
    }
    calibration_ports[index + 1u]  = observed.port;
    calibration_received_mask     |= static_cast<uint8_t>(UINT8_C(1) << (index + 1u));
}

bool NtrsServer::beginCalibration(Registration* registration)
{
    if (registration == NULL) return false;
    registration->calibration_id             = 0u;
    registration->calibration_ip             = registration->endpoint;
    registration->calibration_endpoint_count = calibration_endpoint_count_;
    registration->calibration_ports          = {};
    registration->calibration_ip_consistent  = true;
    registration->calibration_ports[0u]      = registration->endpoint.port;
    registration->calibration_received_mask  = UINT8_C(0x01);
    if (calibration_endpoint_count_ == 0u) return true;
    return random_u64(&registration->calibration_id);
}

static bool appendRegistrationObservedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                                 const Registration& registration, uint8_t public_candidate_count)
{
    std::array<bool, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES> selected = {};

    for (uint8_t count = 0u; count < registration.observed_address_count; ++count) {
        uint8_t  newest    = UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES;
        uint64_t newest_at = 0u;

        for (uint8_t index = 0u; index < registration.observed_address_count; ++index) {
            if (!selected[index] &&
                (newest == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES || registration.observed_at_unix_ms[index] > newest_at)) {
                newest    = index;
                newest_at = registration.observed_at_unix_ms[index];
            }
        }
        if (newest == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) break;
        selected[newest] = true;
        if (plan->public_candidate_count >= public_candidate_count) break;
        if (!append_public_candidate(plan, registration.observed_addresses[newest])) return false;
    }
    return true;
}

static uint16_t advance_port(uint16_t port, int delta)
{
    int value = static_cast<int>(port) - 1 + delta;

    value %= 65535;
    if (value < 0) value += 65535;
    return static_cast<uint16_t>(value + 1);
}

static bool appendRegistrationPredictedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                                  const Registration& registration, uint8_t public_candidate_count)
{
    utp_address_t candidate                = {};
    bool          predictable              = false;
    int           step                     = 0;
    uint8_t       expected_mask            = 0u;
    uint8_t       calibration_sample_count = 0u;

    if (registration.nat_class != UTP_NAT_CLASS_SYMMETRIC && registration.nat_class != UTP_NAT_CLASS_UNKNOWN)
        return true;
    if (!endpoint_to_address(registration.endpoint, &candidate)) return false;
    calibration_sample_count = (uint8_t)(registration.calibration_endpoint_count + 1u);
    if (calibration_sample_count >= 3u) {
        expected_mask = static_cast<uint8_t>((UINT8_C(1) << calibration_sample_count) - 1u);
        if (registration.calibration_ip_consistent && registration.calibration_received_mask == expected_mask) {
            predictable = true;
            for (uint8_t index = 1u; index < calibration_sample_count; ++index) {
                int current_step = static_cast<int>(registration.calibration_ports[index]) -
                                   static_cast<int>(registration.calibration_ports[index - 1u]);

                if (current_step > 32767)
                    current_step -= 65535;
                else if (current_step < -32767)
                    current_step += 65535;
                if (current_step == 0 || current_step < -1024 || current_step > 1024 ||
                    (index > 1u && current_step != step)) {
                    predictable = false;
                    break;
                }
                step = current_step;
            }
        }
    }
    if (predictable) {
        candidate.port = registration.calibration_ports[calibration_sample_count - 1u];
        while (plan->public_candidate_count < public_candidate_count) {
            candidate.port = advance_port(candidate.port, step);
            if (!append_public_candidate(plan, candidate)) return false;
        }
    }
    while (plan->public_candidate_count < public_candidate_count) {
        if (!random_port(&candidate.port) || !append_public_candidate(plan, candidate)) return false;
    }
    return true;
}

static bool appendTemporaryPredictedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                               const PendingRendezvous& transaction, uint8_t public_candidate_count)
{
    utp_address_t candidate = {};
    int           first_step;
    int           second_step;

    if (!endpoint_to_address(transaction.calibration_ip, &candidate)) return false;
    first_step =
        static_cast<int>(transaction.calibration_ports[1u]) - static_cast<int>(transaction.calibration_ports[0u]);
    second_step =
        static_cast<int>(transaction.calibration_ports[2u]) - static_cast<int>(transaction.calibration_ports[1u]);
    if (first_step > 32767)
        first_step -= 65535;
    else if (first_step < -32767)
        first_step += 65535;
    if (second_step > 32767)
        second_step -= 65535;
    else if (second_step < -32767)
        second_step += 65535;
    if (!transaction.calibration_ip_consistent || transaction.calibration_received_mask != UINT8_C(0x07) ||
        first_step == 0 || first_step != second_step || first_step < -1024 || first_step > 1024) {
        while (plan->public_candidate_count < public_candidate_count) {
            if (!random_port(&candidate.port) || !append_public_candidate(plan, candidate)) return false;
        }
        return true;
    }
    candidate.port = transaction.calibration_ports[2u];
    while (plan->public_candidate_count < public_candidate_count) {
        candidate.port = advance_port(candidate.port, first_step);
        if (!append_public_candidate(plan, candidate)) return false;
    }
    return true;
}

static std::string make_endpoint_text(const std::string& address, uint16_t port)
{
    const std::string port_text = std::to_string(port);

    if (!address.empty() && address[0] == '[') return address + ":" + port_text;
    if (address.find(':') != std::string::npos) return "[" + address + "]:" + port_text;
    return address + ":" + port_text;
}

struct OutgoingFrame {
    const uint8_t* body;
    size_t         body_length;
    uint8_t        message_type;
};

bool NtrsServer::sendDatagram(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                              const uint8_t* packet, size_t packet_length)
{
    if (socket == NULL || socket->fd < 0 || fatal_socket_error_ || packet == NULL || packet_length == 0u ||
        packet_length > UTP_PACKET_MTU_FLOOR)
        return false;
    if (socket->output_queue.empty()) {
        ssize_t sent_length;
        for (;;) {
            sent_length =
                sendto(socket->fd, packet, packet_length, 0, reinterpret_cast<const sockaddr*>(&peer), peer_length);
            if (sent_length == static_cast<ssize_t>(packet_length)) return true;
            if (sent_length >= 0 || errno != EINTR) break;
        }
        if (sent_length >= 0) {
            fprintf(stderr, "NTRS [DatagramDropped] udp send short=%zd\n", sent_length);
            return false;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (is_fatal_udp_send_error(errno))
                stopForUdpSendError(errno);
            else
                fprintf(stderr, "NTRS [DatagramDropped] udp send errno=%d\n", errno);
            return false;
        }
    }
    if (socket->output_queue.size() >= k_output_queue_capacity || socket->write_event == NULL) return false;
    OutgoingDatagram datagram = {};
    datagram.peer             = peer;
    datagram.peer_length      = peer_length;
    datagram.packet_length    = packet_length;
    memcpy(datagram.packet.data(), packet, packet_length);
    socket->output_queue.push_back(datagram);
    if (!socket->write_event_active) {
        if (event_add(socket->write_event, NULL) != 0) {
            socket->output_queue.pop_back();
            return false;
        }
        socket->write_event_active = true;
    }
    return true;
}

bool NtrsServer::sendFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                            const OutgoingFrame* frames, size_t frame_count, uint64_t* packet_number,
                            std::array<uint8_t, UTP_PACKET_MTU_FLOOR>* encoded_packet, size_t* encoded_packet_length)
{
    uint8_t             packet[UTP_PACKET_MTU_FLOOR] = {};
    utp_packet_header_t header                       = {};
    size_t              payload_length               = 0u;
    size_t              offset                       = UTP_PACKET_HEADER_SIZE;

    if (frames == NULL || frame_count == 0u || next_packet_number_ == 0u || next_packet_number_ > UTP_PACKET_NUMBER_MAX)
        return false;
    for (size_t index = 0u; index < frame_count; ++index) {
        if (frames[index].message_type == 0u || frames[index].body_length > UINT16_MAX ||
            (frames[index].body == NULL && frames[index].body_length != 0u) ||
            payload_length > UINT16_MAX - UTP_FRAME_RENDEZVOUS_HEADER_SIZE - frames[index].body_length)
            return false;
        payload_length += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
    }
    if (UTP_PACKET_HEADER_SIZE + payload_length > sizeof(packet)) return false;
    header.packet_number  = next_packet_number_++;
    header.payload_length = static_cast<uint16_t>(payload_length);
    header.type           = UTP_PACKET_TYPE_RENDEZVOUS;
    if (utp_proto_encode_header(packet, sizeof(packet), &header) != UTP_INTERNAL_ERROR_OK) return false;
    for (size_t index = 0u; index < frame_count; ++index) {
        const utp_frame_rendezvous_t frame = {frames[index].body, static_cast<uint16_t>(frames[index].body_length),
                                              frames[index].message_type};
        if (utp_frame_rendezvous_encode(packet + offset, sizeof(packet) - offset, &frame) != UTP_INTERNAL_ERROR_OK)
            return false;
        offset += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
    }
    if (packet_number != NULL) *packet_number = header.packet_number;
    if (encoded_packet != NULL && encoded_packet_length != NULL) {
        memcpy(encoded_packet->data(), packet, offset);
        *encoded_packet_length = offset;
    }
    return sendDatagram(socket, peer, peer_length, packet, offset);
}

bool NtrsServer::sendMessage(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                             uint8_t message_type, const uint8_t* body, size_t body_length)
{
    const OutgoingFrame frame = {body, body_length, message_type};

    return sendFrames(socket, peer, peer_length, &frame, 1u, NULL);
}

bool NtrsServer::sendRejected(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                              uint8_t rejected_message_type, const uint8_t* reference_id, uint8_t reference_length,
                              uint16_t reason_code)
{
    utp_rendezvous_rejected_t rejected                          = {};
    uint8_t                   body[UTP_RENDEZVOUS_ID_SIZE + 4u] = {};
    size_t                    body_length                       = 0u;

    if (reference_id == NULL || reference_length > sizeof(rejected.reference_id)) return false;
    rejected.rejected_message_type = rejected_message_type;
    rejected.reference_length      = reference_length;
    rejected.reason_code           = reason_code;
    memcpy(rejected.reference_id, reference_id, reference_length);
    if (utp_rendezvous_rejected_encode(body, sizeof(body), &rejected, &body_length) != UTP_INTERNAL_ERROR_OK)
        return false;
    return sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REJECTED, body, body_length);
}

bool NtrsServer::sendKeepalivePing(Registration* registration, uint64_t now_ms)
{
    utp_rendezvous_ping_t ping                                                            = {};
    uint8_t               body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    sockaddr_storage      peer                                                            = {};
    socklen_t             peer_length                                                     = 0u;
    OutgoingFrame         frame;
    uint64_t              packet_number = 0u;

    if (registration == NULL || !utp_ntrs_endpoint_to_sockaddr(&registration->endpoint, &peer, &peer_length)) {
        return false;
    }
    memcpy(ping.registration_token, registration->token.data(), sizeof(ping.registration_token));
    if (utp_rendezvous_ping_encode(body, sizeof(body), &ping) != UTP_INTERNAL_ERROR_OK) return false;
    frame = OutgoingFrame{body, sizeof(body), UTP_RENDEZVOUS_MESSAGE_PING};
    if (!sendFrames(&primary_socket_, peer, peer_length, &frame, 1u, &packet_number, NULL, NULL)) return false;
    registration->keepalive_packet_number = packet_number;
    registration->last_keepalive_ping_ms  = now_ms;
    return true;
}

bool NtrsServer::sendCalibration(PendingRendezvous* transaction)
{
    utp_rendezvous_calibrate_t calibrate                  = {};
    uint8_t                    body[UTP_PACKET_MTU_FLOOR] = {};
    size_t                     body_length                = 0u;

    if (transaction == NULL || !transaction->calibration_pending || calibration_endpoint_count_ < 2u ||
        transaction->calibration_id == 0u) {
        return false;
    }
    memcpy(calibrate.rendezvous_id, transaction->rendezvous_id.data(), sizeof(calibrate.rendezvous_id));
    memcpy(calibrate.calibration_token, transaction->calibration_token.data(), sizeof(calibrate.calibration_token));
    calibrate.calibration_id = transaction->calibration_id;
    calibrate.endpoint_count = k_temporary_calibration_endpoint_count;
    calibrate.endpoints      = calibration_endpoints_.data();
    if (utp_rendezvous_calibrate_encode(body, sizeof(body), &calibrate, &body_length) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    return sendMessage(&primary_socket_, transaction->source_socket_address, transaction->source_socket_length,
                       UTP_RENDEZVOUS_MESSAGE_CALIBRATE, body, body_length);
}

bool NtrsServer::completePendingRendezvous(PendingRendezvous* transaction)
{
    utp_rendezvous_candidate_plan_t source_plan                                                          = {};
    utp_rendezvous_forward_t        forward                                                              = {};
    utp_rendezvous_ping_t           ping                                                                 = {};
    uint8_t                         forward_body[UTP_PACKET_MTU_FLOOR]                                   = {};
    uint8_t                         ping_body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    size_t                          forward_body_length                                                  = 0u;

    if (transaction == NULL || transaction->source_peer_id_length == 0u ||
        !make_candidate_plan(
            &source_plan, transaction->source_local_family, transaction->source_local_port,
            transaction->source_local_candidates.data(), transaction->source_local_candidate_count,
            transaction->has_source_reported_public_endpoint ? &transaction->source_reported_public_endpoint : NULL,
            transaction->source_endpoint, public_candidate_count_)) {
        return false;
    }
    if (transaction->calibration_id != 0u &&
        !appendTemporaryPredictedCandidates(&source_plan, *transaction, public_candidate_count_)) {
        return false;
    }
    forward.source_peer_id        = transaction->source_peer_id.data();
    forward.source_peer_id_length = transaction->source_peer_id_length;
    memcpy(forward.rendezvous_id, transaction->rendezvous_id.data(), sizeof(forward.rendezvous_id));
    memcpy(forward.punch_token, transaction->punch_token.data(), sizeof(forward.punch_token));
    forward.source_plan = source_plan;
    if (utp_rendezvous_forward_encode(forward_body, sizeof(forward_body), &forward, &forward_body_length) !=
        UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    memcpy(ping.registration_token, transaction->target_token.data(), sizeof(ping.registration_token));
    if (utp_rendezvous_ping_encode(ping_body, sizeof(ping_body), &ping) != UTP_INTERNAL_ERROR_OK) return false;
    transaction->calibration_pending       = false;
    transaction->forward_retry_at_ms       = utp_ntrs_now_ms() + k_forward_retry_initial_delay_ms;
    transaction->forward_retries_remaining = k_forward_retry_count;
    transaction->forward_retry_delay_ms    = k_forward_retry_initial_delay_ms;
    const OutgoingFrame frames[2]          = {{ping_body, sizeof(ping_body), UTP_RENDEZVOUS_MESSAGE_PING},
                                              {forward_body, forward_body_length, UTP_RENDEZVOUS_MESSAGE_FORWARD}};
    if (!sendMessage(&primary_socket_, transaction->source_socket_address, transaction->source_socket_length,
                     UTP_RENDEZVOUS_MESSAGE_REDIRECT, transaction->redirect_body.data(),
                     transaction->redirect_body_length)) {
        return false;
    }
    if (!sendFrames(&primary_socket_, transaction->target_socket_address, transaction->target_socket_length, frames, 2u,
                    &transaction->forward_packet_number, &transaction->forward_packet,
                    &transaction->forward_packet_length)) {
        return false;
    }
    return true;
}

void NtrsServer::expirePendingRendezvous(uint64_t now_ms)
{
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end();) {
        if (now_ms - entry->second.created_at_ms >= k_pending_rendezvous_lifetime_ms)
            entry = pending_rendezvous_.erase(entry);
        else
            ++entry;
    }
}

void NtrsServer::expireRegistrations(uint64_t now_ms)
{
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end();) {
        if (now_ms - entry->second.last_activity_ms >= registration_timeout_ms_)
            entry = registrations_.erase(entry);
        else
            ++entry;
    }
    for (std::unordered_map<std::string, uint64_t>::iterator entry = unregistration_tombstones_.begin();
         entry != unregistration_tombstones_.end();) {
        if (entry->second <= now_ms)
            entry = unregistration_tombstones_.erase(entry);
        else
            ++entry;
    }
}

void NtrsServer::sendKeepalivePings(uint64_t now_ms)
{
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end(); ++entry) {
        Registration& registration = entry->second;

        if (now_ms - registration.last_activity_ms < keepalive_interval_ms_ ||
            (registration.last_keepalive_ping_ms != 0u &&
             now_ms - registration.last_keepalive_ping_ms < keepalive_interval_ms_)) {
            continue;
        }
        (void)sendKeepalivePing(&registration, now_ms);
    }
}

void NtrsServer::retryPendingRendezvous(uint64_t now_ms)
{
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end(); ++entry) {
        PendingRendezvous& transaction = entry->second;

        if (transaction.forward_delivered || transaction.forward_retries_remaining == 0u ||
            transaction.forward_retry_at_ms > now_ms)
            continue;
        if (!sendDatagram(&primary_socket_, transaction.target_socket_address, transaction.target_socket_length,
                          transaction.forward_packet.data(), transaction.forward_packet_length)) {
            transaction.forward_retry_at_ms = now_ms + k_forward_retry_send_failure_delay_ms;
            continue;
        }
        --transaction.forward_retries_remaining;
        transaction.forward_retry_at_ms = now_ms + transaction.forward_retry_delay_ms;
        if (transaction.forward_retry_delay_ms < k_forward_retry_max_delay_ms / 2u)
            transaction.forward_retry_delay_ms *= 2u;
        else
            transaction.forward_retry_delay_ms = k_forward_retry_max_delay_ms;
    }
}

void NtrsServer::handleRegister(const sockaddr_storage& peer, socklen_t peer_length,
                                const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_register_t request                              = {};
    uint8_t                   body[UTP_PACKET_MTU_FLOOR]           = {};
    char                      observed_text[INET6_ADDRSTRLEN + 8u] = {};
    size_t                    body_length                          = 0u;
    bool                      initial_retry                        = false;
    bool                      registration_created                 = false;
    bool                      begin_calibration                    = false;

    if (utp_rendezvous_register_decode(&request, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family != AF_INET) ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family != AF_INET6))
        return;
    const std::string peer_id(reinterpret_cast<const char*>(request.peer_id), request.peer_id_length);
    std::unordered_map<std::string, Registration>::iterator entry = registrations_.find(peer_id);
    if (entry == registrations_.end()) {
        if (!token_is_zero(request.registration_token)) {
            (void)sendRejected(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REGISTER, frame.payload,
                               UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE, k_rejection_token_invalid);
            return;
        }
        Registration registration = {};
        if (!random_token(&registration.token)) return;
        registration.last_register_request_id  = request.registration_request_id;
        registration.last_register_was_initial = true;
        entry                                  = registrations_.insert(std::make_pair(peer_id, registration)).first;
        registration_created                   = true;
    } else {
        initial_retry = token_is_zero(request.registration_token) && entry->second.last_register_was_initial &&
                        request.registration_request_id == entry->second.last_register_request_id;
        const bool update =
            !token_is_zero(request.registration_token) && entry->second.matchesToken(request.registration_token);

        if (!initial_retry && !update) {
            (void)sendRejected(
                &primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REGISTER, frame.payload,
                UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE,
                token_is_zero(request.registration_token) ? k_rejection_peer_id_exists : k_rejection_token_invalid);
            return;
        }
        if (update) {
            entry->second.last_register_request_id  = request.registration_request_id;
            entry->second.last_register_was_initial = false;
        }
    }
    begin_calibration =
        request.nat_class == UTP_NAT_CLASS_SYMMETRIC && (registration_created || entry->second.calibration_id == 0u);
    entry->second.update(request, observed);
    if (begin_calibration && !beginCalibration(&entry->second)) return;
    if (request.nat_class != UTP_NAT_CLASS_SYMMETRIC) {
        entry->second.calibration_id             = 0u;
        entry->second.calibration_ip             = {};
        entry->second.calibration_endpoint_count = 0u;
        entry->second.calibration_received_mask  = 0u;
        entry->second.calibration_ports          = {};
        entry->second.calibration_ip_consistent  = false;
    }
    utp_rendezvous_registered_t reply = {};
    reply.registration_request_id     = request.registration_request_id;
    memcpy(reply.registration_token, entry->second.token.data(), entry->second.token.size());
    if (request.nat_class == UTP_NAT_CLASS_SYMMETRIC && (begin_calibration || initial_retry)) {
        reply.calibration_id             = entry->second.calibration_id;
        reply.calibration_endpoint_count = entry->second.calibration_endpoint_count;
        reply.calibration_endpoints      = calibration_endpoints_.data();
    }
    if (utp_rendezvous_registered_encode(body, sizeof(body), &reply, &body_length) == UTP_INTERNAL_ERROR_OK)
        (void)sendMessage(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REGISTERED, body, body_length);
    fprintf(stderr, "NTRS <- Node=%s [Register] peer_id=%s\n",
            utp_ntrs_endpoint_format(&observed, observed_text, sizeof(observed_text)), peer_id.c_str());
}

void NtrsServer::handlePing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                            const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                            const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_ping_t ping                                                            = {};
    utp_rendezvous_pong_t pong                                                            = {};
    uint8_t               body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};

    if (utp_rendezvous_ping_decode(&ping, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK) return;
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end(); ++entry) {
        if (!entry->second.matchesToken(ping.registration_token)) continue;
        if (!entry->second.acceptsFamily(observed)) return;
        if (socket->calibration_index == k_primary_socket_index) {
            if (ping.calibration_id != 0u) return;
            entry->second.updateEndpoint(observed);
        } else {
            if (ping.calibration_id == 0u || ping.calibration_id != entry->second.calibration_id ||
                socket->calibration_index >= entry->second.calibration_endpoint_count)
                return;
            entry->second.recordCalibrationEndpoint(socket->calibration_index, observed);
        }
        entry->second.touch(utp_ntrs_now_ms());
        memcpy(pong.registration_token, ping.registration_token, sizeof(pong.registration_token));
        pong.acknowledged_packet_number = header.packet_number;
        if (utp_rendezvous_pong_encode(body, sizeof(body), &pong) == UTP_INTERNAL_ERROR_OK)
            (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
        return;
    }
    if (socket->calibration_index == k_primary_socket_index || ping.calibration_id == 0u ||
        socket->calibration_index >= k_temporary_calibration_endpoint_count) {
        return;
    }
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end(); ++entry) {
        PendingRendezvous& transaction = entry->second;
        const uint8_t      index       = (uint8_t)(socket->calibration_index + 1u);

        if (!transaction.calibration_pending || transaction.calibration_id != ping.calibration_id ||
            memcmp(transaction.calibration_token.data(), ping.registration_token,
                   transaction.calibration_token.size()) != 0) {
            continue;
        }
        if (!endpoint_ips_equal(transaction.calibration_ip, observed)) {
            transaction.calibration_ip_consistent = false;
            return;
        }
        transaction.calibration_ports[index]   = observed.port;
        transaction.calibration_received_mask |= (uint8_t)(UINT8_C(1) << index);
        memcpy(pong.registration_token, ping.registration_token, sizeof(pong.registration_token));
        pong.acknowledged_packet_number = header.packet_number;
        if (utp_rendezvous_pong_encode(body, sizeof(body), &pong) == UTP_INTERNAL_ERROR_OK)
            (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
        if (transaction.calibration_received_mask == UINT8_C(0x07)) (void)completePendingRendezvous(&transaction);
        return;
    }
}

void NtrsServer::handlePong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_pong_t pong = {};

    if (utp_rendezvous_pong_decode(&pong, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK) return;
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end(); ++entry) {
        Registration& registration = entry->second;

        if (endpoints_equal(registration.endpoint, observed) && registration.matchesToken(pong.registration_token) &&
            registration.keepalive_packet_number == pong.acknowledged_packet_number) {
            registration.keepalive_packet_number = 0u;
            registration.last_keepalive_ping_ms  = 0u;
            registration.touch(utp_ntrs_now_ms());
            return;
        }
    }
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end(); ++entry) {
        if (endpoints_equal(entry->second.target_endpoint, observed) &&
            memcmp(entry->second.target_token.data(), pong.registration_token, entry->second.target_token.size()) ==
                0 &&
            entry->second.forward_packet_number == pong.acknowledged_packet_number) {
            entry->second.forward_delivered = true;
            for (std::unordered_map<std::string, Registration>::iterator registration = registrations_.begin();
                 registration != registrations_.end(); ++registration) {
                if (endpoints_equal(registration->second.endpoint, observed) &&
                    registration->second.matchesToken(pong.registration_token)) {
                    registration->second.touch(utp_ntrs_now_ms());
                    break;
                }
            }
            return;
        }
    }
}

void NtrsServer::handleAddressUpdate(const sockaddr_storage& peer, socklen_t peer_length,
                                     const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_address_update_t  update                               = {};
    utp_rendezvous_address_updated_t updated                              = {};
    uint8_t                          body[sizeof(updated.update_id)]      = {};
    uint8_t                          accepted                             = 0u;
    size_t                           body_length                          = 0u;
    char                             observed_text[INET6_ADDRSTRLEN + 8u] = {};

    if (utp_rendezvous_address_update_decode(&update, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end(); ++entry) {
        if (!entry->second.matchesToken(update.registration_token)) continue;
        if (!endpoints_equal(entry->second.endpoint, observed)) return;
        accepted = entry->second.updateObservedAddresses(update);
        entry->second.touch(utp_ntrs_now_ms());
        updated.update_id = update.update_id;
        if (utp_rendezvous_address_updated_encode(body, sizeof(body), &updated) != UTP_INTERNAL_ERROR_OK) return;
        body_length = sizeof(body);
        (void)sendMessage(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_ADDRESS_UPDATED, body,
                          body_length);
        fprintf(stderr, "NTRS <- Node=%s [AddressUpdate] samples=%u accepted=%u\n",
                utp_ntrs_endpoint_format(&observed, observed_text, sizeof(observed_text)),
                static_cast<unsigned>(update.sample_count), static_cast<unsigned>(accepted));
        return;
    }
}

void NtrsServer::handleUnregister(const sockaddr_storage& peer, socklen_t peer_length,
                                  const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_unregister_t unregister_message = {};
    const uint64_t              now_ms             = utp_ntrs_now_ms();

    if (utp_rendezvous_unregister_decode(&unregister_message, frame.payload, frame.payload_length) !=
        UTP_INTERNAL_ERROR_OK)
        return;
    const std::string key(reinterpret_cast<const char*>(unregister_message.registration_token),
                          sizeof(unregister_message.registration_token));
    for (std::unordered_map<std::string, Registration>::iterator entry = registrations_.begin();
         entry != registrations_.end(); ++entry) {
        if (entry->second.matchesToken(unregister_message.registration_token)) {
            for (std::unordered_map<std::string, PendingRendezvous>::iterator pending = pending_rendezvous_.begin();
                 pending != pending_rendezvous_.end();) {
                if (memcmp(pending->second.target_token.data(), unregister_message.registration_token,
                           sizeof(unregister_message.registration_token)) == 0)
                    pending = pending_rendezvous_.erase(pending);
                else
                    ++pending;
            }
            registrations_.erase(entry);
            unregistration_tombstones_[key] = now_ms + k_unregistration_tombstone_lifetime_ms;
            (void)sendMessage(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_UNREGISTERED,
                              unregister_message.registration_token, sizeof(unregister_message.registration_token));
            return;
        }
    }
    if (unregistration_tombstones_.find(key) != unregistration_tombstones_.end()) {
        (void)sendMessage(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_UNREGISTERED,
                          unregister_message.registration_token, sizeof(unregister_message.registration_token));
        return;
    }
    (void)sendRejected(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_UNREGISTER,
                       unregister_message.registration_token, sizeof(unregister_message.registration_token),
                       k_rejection_token_invalid);
}

void NtrsServer::handleRequest(const sockaddr_storage& peer, socklen_t peer_length, const utp_ntrs_endpoint_t& observed,
                               const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_request_t        request                            = {};
    utp_rendezvous_candidate_plan_t target_plan                        = {};
    utp_rendezvous_redirect_t       redirect                           = {};
    sockaddr_storage                target_address                     = {};
    socklen_t                       target_length                      = 0u;
    char                            source_text[INET6_ADDRSTRLEN + 8u] = {};
    char                            target_text[INET6_ADDRSTRLEN + 8u] = {};

    if (utp_rendezvous_request_decode(&request, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family != AF_INET) ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family != AF_INET6))
        return;
    expirePendingRendezvous(utp_ntrs_now_ms());
    const std::string rendezvous_key(reinterpret_cast<const char*>(request.rendezvous_id),
                                     sizeof(request.rendezvous_id));
    std::unordered_map<std::string, PendingRendezvous>::iterator pending = pending_rendezvous_.find(rendezvous_key);
    if (pending != pending_rendezvous_.end()) {
        if (endpoints_equal(pending->second.source_endpoint, observed)) {
            if (pending->second.calibration_pending)
                (void)sendCalibration(&pending->second);
            else
                (void)sendMessage(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REDIRECT,
                                  pending->second.redirect_body.data(), pending->second.redirect_body_length);
        }
        return;
    }
    const std::string target_peer_id(reinterpret_cast<const char*>(request.target_peer_id),
                                     request.target_peer_id_length);
    std::unordered_map<std::string, Registration>::iterator target = registrations_.find(target_peer_id);
    if (target == registrations_.end() || target->second.local_family != request.local_family ||
        !make_candidate_plan(
            &target_plan, target->second.local_family, target->second.local_port,
            target->second.local_candidates.data(), target->second.local_candidate_count,
            target->second.has_reported_public_endpoint ? &target->second.reported_public_endpoint : NULL,
            target->second.endpoint, public_candidate_count_) ||
        !appendRegistrationObservedCandidates(&target_plan, target->second, public_candidate_count_) ||
        !appendRegistrationPredictedCandidates(&target_plan, target->second, public_candidate_count_) ||
        !utp_ntrs_endpoint_to_sockaddr(&target->second.endpoint, &target_address, &target_length)) {
        (void)sendRejected(&primary_socket_, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REQUEST, request.rendezvous_id,
                           sizeof(request.rendezvous_id), k_rejection_peer_not_found);
        return;
    }

    PendingRendezvous transaction = {};
    if (!random_token(&transaction.punch_token)) return;
    memcpy(transaction.rendezvous_id.data(), request.rendezvous_id, transaction.rendezvous_id.size());
    memcpy(redirect.rendezvous_id, request.rendezvous_id, sizeof(redirect.rendezvous_id));
    memcpy(redirect.punch_token, transaction.punch_token.data(), transaction.punch_token.size());
    redirect.target_plan = target_plan;
    if (utp_rendezvous_redirect_encode(transaction.redirect_body.data(), transaction.redirect_body.size(), &redirect,
                                       &transaction.redirect_body_length) != UTP_INTERNAL_ERROR_OK)
        return;
    transaction.source_endpoint       = observed;
    transaction.target_endpoint       = target->second.endpoint;
    transaction.source_socket_address = peer;
    transaction.source_socket_length  = peer_length;
    transaction.target_socket_address = target_address;
    transaction.target_socket_length  = target_length;
    memcpy(transaction.target_token.data(), target->second.token.data(), transaction.target_token.size());
    memcpy(transaction.source_peer_id.data(), request.source_peer_id, request.source_peer_id_length);
    transaction.source_peer_id_length               = request.source_peer_id_length;
    transaction.source_local_port                   = request.local_port;
    transaction.source_local_family                 = request.local_family;
    transaction.source_local_candidate_count        = request.local_candidate_count;
    transaction.has_source_reported_public_endpoint = request.reported_public_endpoint != NULL;
    if (transaction.has_source_reported_public_endpoint)
        transaction.source_reported_public_endpoint = *request.reported_public_endpoint;
    for (uint8_t index = 0u; index < request.local_candidate_count; ++index)
        transaction.source_local_candidates[index] = request.local_candidates[index];
    transaction.created_at_ms       = utp_ntrs_now_ms();
    transaction.calibration_pending = request.source_nat_class == UTP_NAT_CLASS_SYMMETRIC &&
                                      target->second.nat_class == UTP_NAT_CLASS_PORT_RESTRICTED &&
                                      calibration_endpoint_count_ >= k_temporary_calibration_endpoint_count;
    if (transaction.calibration_pending) {
        transaction.calibration_ip            = observed;
        transaction.calibration_ports[0u]     = observed.port;
        transaction.calibration_received_mask = UINT8_C(0x01);
        transaction.calibration_ip_consistent = true;
        transaction.calibration_deadline_ms   = transaction.created_at_ms + k_temporary_calibration_timeout_ms;
        if (!random_u64(&transaction.calibration_id) || !random_token(&transaction.calibration_token) ||
            !sendCalibration(&transaction)) {
            return;
        }
        pending_rendezvous_.insert(std::make_pair(rendezvous_key, transaction));
        fprintf(stderr, "NTRS <- Node=%s [Request|Calibrate] target=%s\n",
                utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
        return;
    }
    if (!completePendingRendezvous(&transaction)) return;
    pending_rendezvous_.insert(std::make_pair(rendezvous_key, transaction));
    fprintf(stderr, "NTRS <- Node=%s [Request] target_peer_id=%s\n",
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
    fprintf(stderr, "NTRS -> Node=%s [Redirect] target=%s\n",
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
    fprintf(stderr, "NTRS -> Node=%s [Ping|Forward] source=%s\n",
            utp_ntrs_endpoint_format(&target->second.endpoint, target_text, sizeof(target_text)),
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)));
}

void NtrsServer::OnReadEvent(evutil_socket_t, short, void* user_data)
{
    UdpSocket* const socket = static_cast<UdpSocket*>(user_data);

    socket->server->onRead(socket);
}

void NtrsServer::onRead(UdpSocket* socket)
{
    if (socket == NULL || socket->fd < 0) return;
    for (;;) {
        uint8_t          packet[UTP_PACKET_MTU_FLOOR];
        sockaddr_storage peer        = {};
        socklen_t        peer_length = sizeof(peer);
        const ssize_t    count =
            recvfrom(socket->fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        utp_packet_header_t header   = {};
        utp_ntrs_endpoint_t observed = {};

        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) fprintf(stderr, "event=udp_receive_failed errno=%d\n", errno);
            return;
        }
        if (utp_proto_decode_header(&header, packet, static_cast<size_t>(count)) != UTP_INTERNAL_ERROR_OK ||
            header.packet_number == 0u || header.reserve != 0u ||
            static_cast<size_t>(count) != UTP_PACKET_HEADER_SIZE + header.payload_length ||
            !utp_ntrs_endpoint_from_sockaddr(&observed, reinterpret_cast<const sockaddr*>(&peer), peer_length))
            continue;
        if (header.type == UTP_PACKET_TYPE_RENDEZVOUS) {
            utp_frame_rendezvous_t frame = {};
            if (header.scid != 0u || header.dcid != 0u ||
                utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE, header.payload_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                header.payload_length != UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frame.payload_length)
                continue;
            if (socket->calibration_index != k_primary_socket_index &&
                frame.message_type != UTP_RENDEZVOUS_MESSAGE_PING)
                continue;
            if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_REGISTER)
                handleRegister(peer, peer_length, observed, frame);
            else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PING)
                handlePing(socket, peer, peer_length, observed, header, frame);
            else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PONG &&
                     socket->calibration_index == k_primary_socket_index)
                handlePong(observed, frame);
            else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_ADDRESS_UPDATE &&
                     socket->calibration_index == k_primary_socket_index)
                handleAddressUpdate(peer, peer_length, observed, frame);
            else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_UNREGISTER &&
                     socket->calibration_index == k_primary_socket_index)
                handleUnregister(peer, peer_length, frame);
        } else if (socket->calibration_index == k_primary_socket_index &&
                   (header.type == UTP_PACKET_TYPE_INITIAL || header.type == UTP_PACKET_TYPE_0RTT)) {
            utp_frame_rendezvous_t frame        = {};
            uint8_t                frame_type   = 0u;
            size_t                 frame_length = 0u;
            if (header.scid == 0u || header.dcid != 0u ||
                utp_frame_measure(packet + UTP_PACKET_HEADER_SIZE, header.payload_length, &frame_type, &frame_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                frame_type != UTP_FRAME_TYPE_RENDEZVOUS ||
                utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE, frame_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                frame.message_type != UTP_RENDEZVOUS_MESSAGE_REQUEST)
                continue;
            handleRequest(peer, peer_length, observed, frame);
        }
    }
}

static void on_signal(evutil_socket_t, short, void* user_data)
{
    event_base_loopbreak(static_cast<event_base*>(user_data));
}

void NtrsServer::OnTimerEvent(evutil_socket_t, short, void* user_data)
{
    static_cast<NtrsServer*>(user_data)->onTimer();
}

void NtrsServer::onTimer()
{
    const uint64_t now_ms = utp_ntrs_now_ms();

    expirePendingRendezvous(now_ms);
    expireRegistrations(now_ms);
    sendKeepalivePings(now_ms);
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end(); ++entry) {
        PendingRendezvous& transaction = entry->second;

        if (transaction.calibration_pending && transaction.calibration_deadline_ms <= now_ms)
            (void)completePendingRendezvous(&transaction);
    }
    retryPendingRendezvous(now_ms);
}

void NtrsServer::OnWriteEvent(evutil_socket_t, short, void* user_data)
{
    UdpSocket* const socket = static_cast<UdpSocket*>(user_data);

    socket->server->onWrite(socket);
}

void NtrsServer::onWrite(UdpSocket* socket)
{
    if (socket == NULL || socket->fd < 0) return;
    while (!socket->output_queue.empty()) {
        const OutgoingDatagram& datagram = socket->output_queue.front();
        ssize_t                 sent_length;
        do {
            sent_length = sendto(socket->fd, datagram.packet.data(), datagram.packet_length, 0,
                                 reinterpret_cast<const sockaddr*>(&datagram.peer), datagram.peer_length);
        } while (sent_length < 0 && errno == EINTR);
        if (sent_length == static_cast<ssize_t>(datagram.packet_length)) {
            socket->output_queue.pop_front();
            continue;
        }
        if (sent_length >= 0) {
            fprintf(stderr, "NTRS [DatagramDropped] udp send short=%zd\n", sent_length);
            socket->output_queue.pop_front();
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (is_fatal_udp_send_error(errno)) {
            stopForUdpSendError(errno);
            return;
        }
        fprintf(stderr, "NTRS [DatagramDropped] udp send errno=%d\n", errno);
        socket->output_queue.pop_front();
    }
    event_del(socket->write_event);
    socket->write_event_active = false;
}

bool NtrsServer::start(const utp_ntrs_endpoint_t& bind_endpoint, const utp_ntrs_endpoint_t& advertised_endpoint,
                       const char* interface_name, const std::vector<uint16_t>& calibration_ports,
                       uint8_t public_candidate_count, uint32_t registration_timeout_ms, uint32_t keepalive_interval_ms)
{
    if (bind_endpoint.family != advertised_endpoint.family || public_candidate_count == 0u ||
        registration_timeout_ms == 0u || keepalive_interval_ms == 0u ||
        keepalive_interval_ms >= registration_timeout_ms ||
        public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        calibration_ports.size() > calibration_sockets_.size() ||
        (!calibration_ports.empty() && endpoint_is_unspecified(advertised_endpoint)) ||
        (base_ = event_base_new()) == NULL ||
        !createSocket(&primary_socket_, bind_endpoint, interface_name, k_primary_socket_index) ||
        (timer_event_ = event_new(base_, -1, EV_PERSIST, OnTimerEvent, this)) == NULL)
        return false;
    public_candidate_count_  = public_candidate_count;
    registration_timeout_ms_ = registration_timeout_ms;
    keepalive_interval_ms_   = keepalive_interval_ms;
    for (size_t index = 0u; index < calibration_ports.size(); ++index) {
        utp_ntrs_endpoint_t calibration_bind_endpoint       = bind_endpoint;
        utp_ntrs_endpoint_t calibration_advertised_endpoint = advertised_endpoint;

        if (calibration_ports[index] == 0u || calibration_ports[index] == bind_endpoint.port) return false;
        for (size_t previous = 0u; previous < index; ++previous) {
            if (calibration_ports[previous] == calibration_ports[index]) return false;
        }
        calibration_bind_endpoint.port       = calibration_ports[index];
        calibration_advertised_endpoint.port = calibration_ports[index];
        if (!createSocket(&calibration_sockets_[index], calibration_bind_endpoint, interface_name,
                          static_cast<uint8_t>(index)) ||
            !endpoint_to_address(calibration_advertised_endpoint, &calibration_endpoints_[index]))
            return false;
        ++calibration_endpoint_count_;
    }
    return true;
}

int NtrsServer::run(const utp_ntrs_endpoint_t& endpoint)
{
    event*        signal_int                                = evsignal_new(base_, SIGINT, on_signal, base_);
    event*        signal_term                               = evsignal_new(base_, SIGTERM, on_signal, base_);
    const timeval timer_interval                            = {0, 100000};
    char          formatted_endpoint[INET6_ADDRSTRLEN + 8u] = {};

    if (signal_int == NULL || signal_term == NULL || event_add(signal_int, NULL) != 0 ||
        event_add(signal_term, NULL) != 0 || event_add(timer_event_, &timer_interval) != 0) {
        if (signal_int != NULL) event_free(signal_int);
        if (signal_term != NULL) event_free(signal_term);
        return 1;
    }
    fprintf(stderr, "NTRS [Started] bind=%s\n",
            utp_ntrs_endpoint_format(&endpoint, formatted_endpoint, sizeof(formatted_endpoint)));
    (void)event_base_dispatch(base_);
    event_free(signal_int);
    event_free(signal_term);
    return fatal_socket_error_ ? 1 : 0;
}

int main(int argc, char** argv)
{
    CLI::App              cli{"NTRS rendezvous service"};
    std::string           bind_address = "0.0.0.0";
    std::string           advertised_address;
    std::string           interface_name;
    std::vector<uint16_t> calibration_ports;
    uint32_t              public_candidate_count  = UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES;
    uint32_t              registration_timeout_ms = k_registration_timeout_default_ms;
    uint32_t              keepalive_interval_ms   = k_keepalive_interval_default_ms;
    uint16_t              port                    = 24000u;
    cli.add_option("-a", bind_address, "Bind IP");
    cli.add_option("-e", advertised_address, "Advertised public IP");
    cli.add_option("-p", port, "Bind UDP port");
    cli.add_option("-i", interface_name, "Bind interface");
    cli.add_option("-c", calibration_ports, "Calibration UDP port");
    cli.add_option("-n", public_candidate_count, "Public candidate count");
    cli.add_option("-t", registration_timeout_ms, "Registration timeout ms");
    cli.add_option("-k", keepalive_interval_ms, "NTRS keepalive interval ms");
    CLI11_PARSE(cli, argc, argv);

    const std::string bind_endpoint_text = make_endpoint_text(bind_address, port);
    const std::string advertised_endpoint_text =
        make_endpoint_text(advertised_address.empty() ? bind_address : advertised_address, port);
    utp_ntrs_endpoint_t bind_endpoint       = {};
    utp_ntrs_endpoint_t advertised_endpoint = {};
    NtrsServer          server;

    if (public_candidate_count == 0u || public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        registration_timeout_ms == 0u || keepalive_interval_ms == 0u ||
        keepalive_interval_ms >= registration_timeout_ms ||
        !utp_ntrs_endpoint_parse(bind_endpoint_text.c_str(), &bind_endpoint) ||
        !utp_ntrs_endpoint_parse(advertised_endpoint_text.c_str(), &advertised_endpoint) ||
        !server.start(bind_endpoint, advertised_endpoint, interface_name.empty() ? NULL : interface_name.c_str(),
                      calibration_ports, static_cast<uint8_t>(public_candidate_count), registration_timeout_ms,
                      keepalive_interval_ms))
        return 1;
    utp_ntrs_app_log_init("ntrs");
    return server.run(bind_endpoint);
}
