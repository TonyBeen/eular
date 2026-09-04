#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <array>
#include <deque>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/util.h>
#include <ntrs/service.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <utils/CLI11.hpp>

#include "app_log.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "rendezvous/rendezvous.h"
#include "service_util.h"

#define fprintf(stream, ...) UTP_NTRS_APP_LOG(stream, __VA_ARGS__)

struct Registration {
  utp_ntrs_endpoint_t endpoint;
  std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE> token;
  std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES>
      local_candidates;
  utp_address_t reported_public_endpoint;
  uint64_t last_register_request_id;
  uint16_t local_port;
  uint8_t nat_class;
  uint8_t local_family;
  uint8_t local_candidate_count;
  bool has_reported_public_endpoint;
  bool last_register_was_initial;
};

struct PendingRendezvous {
  utp_ntrs_endpoint_t source_endpoint;
  utp_ntrs_endpoint_t target_endpoint;
  sockaddr_storage target_socket_address;
  std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE> target_token;
  std::array<uint8_t, UTP_PACKET_MTU_FLOOR> forward_packet;
  std::array<uint8_t, UTP_PACKET_MTU_FLOOR> redirect_body;
  uint64_t created_at_ms;
  uint64_t forward_packet_number;
  uint64_t forward_retry_at_ms;
  size_t forward_packet_length;
  size_t redirect_body_length;
  socklen_t target_socket_length;
  uint8_t forward_retries_remaining;
  uint32_t forward_retry_delay_ms;
  bool forward_delivered;
};

struct OutgoingDatagram {
  sockaddr_storage peer;
  std::array<uint8_t, UTP_PACKET_MTU_FLOOR> packet;
  size_t packet_length;
  socklen_t peer_length;
};

struct Server {
  event_base *base;
  event *read_event;
  event *timer_event;
  event *write_event;
  int fd;
  uint64_t next_packet_number;
  bool write_event_active;
  std::deque<OutgoingDatagram> output_queue;
  std::unordered_map<std::string, Registration> registrations;
  std::unordered_map<std::string, PendingRendezvous> pending_rendezvous;
};

static const uint64_t k_pending_rendezvous_lifetime_ms = 30000u;
static const uint32_t k_forward_retry_initial_delay_ms = 1000u;
static const uint32_t k_forward_retry_max_delay_ms = 8000u;
static const uint32_t k_forward_retry_send_failure_delay_ms = 100u;
static const uint8_t k_forward_retry_count = 3u;
static const size_t k_output_queue_capacity = 128u;

static void on_write(evutil_socket_t, short, void *user_data);

static bool token_is_zero(const uint8_t *token) {
  uint8_t value = 0u;
  for (size_t index = 0u; index < UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE;
       ++index)
    value |= token[index];
  return value == 0u;
}

static bool random_token(
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE> *token) {
  size_t offset = 0u;
  while (offset < token->size()) {
    const ssize_t count =
        getrandom(token->data() + offset, token->size() - offset, 0);
    if (count > 0)
      offset += static_cast<size_t>(count);
    else if (count < 0 && errno == EINTR)
      continue;
    else
      return false;
  }
  return !token_is_zero(token->data());
}

static bool endpoint_to_address(const utp_ntrs_endpoint_t &endpoint,
                                utp_address_t *address) {
  size_t address_length = 0u;

  if (endpoint.family == AF_INET) {
    address->family = UTP_ADDRESS_FAMILY_IPV4;
    address_length = 4u;
  } else if (endpoint.family == AF_INET6) {
    address->family = UTP_ADDRESS_FAMILY_IPV6;
    address_length = 16u;
  } else {
    return false;
  }
  address->port = endpoint.port;
  memcpy(address->address, endpoint.address, address_length);
  return endpoint.port != 0u;
}

static bool endpoints_equal(const utp_ntrs_endpoint_t &left,
                            const utp_ntrs_endpoint_t &right) {
  const size_t address_length = left.family == AF_INET ? 4u :
                                left.family == AF_INET6 ? 16u : 0u;

  return address_length != 0u && left.family == right.family &&
         left.port == right.port &&
         memcmp(left.address, right.address, address_length) == 0;
}

static bool addresses_equal(const utp_address_t &left,
                            const utp_address_t &right) {
  const size_t address_length =
      left.family == UTP_ADDRESS_FAMILY_IPV4 ? 4u :
      left.family == UTP_ADDRESS_FAMILY_IPV6 ? 16u : 0u;

  return address_length != 0u && left.family == right.family &&
         left.port == right.port &&
         memcmp(left.address, right.address, address_length) == 0;
}

static bool append_public_candidate(
    utp_rendezvous_candidate_plan_t *plan, const utp_address_t &candidate) {
  for (uint8_t index = 0u; index < plan->public_candidate_count; ++index) {
    if (addresses_equal(plan->decoded_public_candidates[index], candidate))
      return true;
  }
  if (plan->public_candidate_count >= UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES)
    return false;
  plan->decoded_public_candidates[plan->public_candidate_count++] = candidate;
  return true;
}

static bool make_candidate_plan(
    utp_rendezvous_candidate_plan_t *plan, uint8_t family, uint16_t local_port,
    const utp_address_t *local_candidates, uint8_t local_candidate_count,
    const utp_address_t *reported_public_endpoint,
    const utp_ntrs_endpoint_t &observed_endpoint) {
  utp_address_t observed_public_endpoint = {};

  if (plan == NULL || local_port == 0u ||
      local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
      !endpoint_to_address(observed_endpoint, &observed_public_endpoint) ||
      observed_public_endpoint.family != family ||
      (local_candidate_count != 0u && local_candidates == NULL))
    return false;
  *plan = {};
  plan->family = family;
  plan->local_port = local_port;
  plan->local_candidates = local_candidates;
  plan->local_candidate_count = local_candidate_count;
  if (reported_public_endpoint != NULL &&
      (reported_public_endpoint->family != family ||
       reported_public_endpoint->port == 0u))
    return false;
  if (reported_public_endpoint != NULL &&
      !append_public_candidate(plan, *reported_public_endpoint))
    return false;
  if (!append_public_candidate(plan, observed_public_endpoint))
    return false;
  plan->public_candidates = plan->decoded_public_candidates;
  return plan->public_candidate_count != 0u;
}

static void update_registration(Registration *registration,
                                const utp_rendezvous_register_t &request,
                                const utp_ntrs_endpoint_t &observed) {
  registration->endpoint = observed;
  registration->nat_class = request.nat_class;
  registration->local_family = request.local_family;
  registration->local_port = request.local_port;
  registration->local_candidate_count = request.local_candidate_count;
  registration->has_reported_public_endpoint =
      request.reported_public_endpoint != NULL;
  if (registration->has_reported_public_endpoint)
    registration->reported_public_endpoint = *request.reported_public_endpoint;
  for (uint8_t index = 0u; index < request.local_candidate_count; ++index)
    registration->local_candidates[index] = request.local_candidates[index];
}

static std::string make_endpoint_text(const std::string &address,
                                      uint16_t port) {
  const std::string port_text = std::to_string(port);

  if (!address.empty() && address[0] == '[')
    return address + ":" + port_text;
  if (address.find(':') != std::string::npos)
    return "[" + address + "]:" + port_text;
  return address + ":" + port_text;
}

struct OutgoingFrame {
  const uint8_t *body;
  size_t body_length;
  uint8_t message_type;
};

static bool send_datagram(Server *server, const sockaddr_storage &peer,
                          socklen_t peer_length, const uint8_t *packet,
                          size_t packet_length) {
  if (server == NULL || packet == NULL || packet_length == 0u ||
      packet_length > UTP_PACKET_MTU_FLOOR)
    return false;
  if (server->output_queue.empty()) {
    if (sendto(server->fd, packet, packet_length, 0,
               reinterpret_cast<const sockaddr *>(&peer), peer_length) ==
        static_cast<ssize_t>(packet_length))
      return true;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      return false;
  }
  if (server->output_queue.size() >= k_output_queue_capacity ||
      server->write_event == NULL)
    return false;
  OutgoingDatagram datagram = {};
  datagram.peer = peer;
  datagram.peer_length = peer_length;
  datagram.packet_length = packet_length;
  memcpy(datagram.packet.data(), packet, packet_length);
  server->output_queue.push_back(datagram);
  if (!server->write_event_active) {
    if (event_add(server->write_event, NULL) != 0)
      return false;
    server->write_event_active = true;
  }
  return true;
}

static bool send_frames(Server *server, const sockaddr_storage &peer,
                        socklen_t peer_length, const OutgoingFrame *frames,
                        size_t frame_count, uint64_t *packet_number,
                        std::array<uint8_t, UTP_PACKET_MTU_FLOOR> *encoded_packet = NULL,
                        size_t *encoded_packet_length = NULL) {
  uint8_t packet[UTP_PACKET_MTU_FLOOR] = {};
  utp_packet_header_t header = {};
  size_t payload_length = 0u;
  size_t offset = UTP_PACKET_HEADER_SIZE;

  if (server == NULL || frames == NULL || frame_count == 0u ||
      server->next_packet_number == 0u ||
      server->next_packet_number > UTP_PACKET_NUMBER_MAX)
    return false;
  for (size_t index = 0u; index < frame_count; ++index) {
    if (frames[index].message_type == 0u || frames[index].body_length > UINT16_MAX ||
        (frames[index].body == NULL && frames[index].body_length != 0u) ||
        payload_length > UINT16_MAX - UTP_FRAME_RENDEZVOUS_HEADER_SIZE - frames[index].body_length)
      return false;
    payload_length += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
  }
  if (UTP_PACKET_HEADER_SIZE + payload_length > sizeof(packet))
    return false;
  header.packet_number = server->next_packet_number++;
  header.payload_length = static_cast<uint16_t>(payload_length);
  header.type = UTP_PACKET_TYPE_RENDEZVOUS;
  if (utp_proto_encode_header(packet, sizeof(packet), &header) !=
      UTP_INTERNAL_ERROR_OK)
    return false;
  for (size_t index = 0u; index < frame_count; ++index) {
    const utp_frame_rendezvous_t frame = {
        frames[index].body, static_cast<uint16_t>(frames[index].body_length),
        frames[index].message_type};
    if (utp_frame_rendezvous_encode(packet + offset, sizeof(packet) - offset,
                                    &frame) != UTP_INTERNAL_ERROR_OK)
      return false;
    offset += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
  }
  if (packet_number != NULL)
    *packet_number = header.packet_number;
  if (encoded_packet != NULL && encoded_packet_length != NULL) {
    memcpy(encoded_packet->data(), packet, offset);
    *encoded_packet_length = offset;
  }
  return send_datagram(server, peer, peer_length, packet, offset);
}

static bool send_message(Server *server, const sockaddr_storage &peer,
                         socklen_t peer_length, uint8_t message_type,
                         const uint8_t *body, size_t body_length) {
  const OutgoingFrame frame = {body, body_length, message_type};

  return send_frames(server, peer, peer_length, &frame, 1u, NULL);
}

static void expire_pending_rendezvous(Server *server, uint64_t now_ms) {
  for (std::unordered_map<std::string, PendingRendezvous>::iterator entry =
           server->pending_rendezvous.begin();
       entry != server->pending_rendezvous.end();) {
    if (now_ms - entry->second.created_at_ms >=
        k_pending_rendezvous_lifetime_ms)
      entry = server->pending_rendezvous.erase(entry);
    else
      ++entry;
  }
}

static void retry_pending_rendezvous(Server *server, uint64_t now_ms) {
  for (std::unordered_map<std::string, PendingRendezvous>::iterator entry =
           server->pending_rendezvous.begin();
       entry != server->pending_rendezvous.end(); ++entry) {
    PendingRendezvous &transaction = entry->second;

    if (transaction.forward_delivered ||
        transaction.forward_retries_remaining == 0u ||
        transaction.forward_retry_at_ms > now_ms)
      continue;
    if (!send_datagram(server, transaction.target_socket_address,
                       transaction.target_socket_length,
                       transaction.forward_packet.data(),
                       transaction.forward_packet_length)) {
      transaction.forward_retry_at_ms =
          now_ms + k_forward_retry_send_failure_delay_ms;
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

static void handle_register(Server *server, const sockaddr_storage &peer,
                            socklen_t peer_length,
                            const utp_ntrs_endpoint_t &observed,
                            const utp_frame_rendezvous_t &frame) {
  utp_rendezvous_register_t request = {};
  uint8_t body[64] = {};
  char observed_text[INET6_ADDRSTRLEN + 8u] = {};
  size_t body_length = 0u;

  if (utp_rendezvous_register_decode(&request, frame.payload,
                                     frame.payload_length) !=
          UTP_INTERNAL_ERROR_OK ||
      (request.local_family == UTP_ADDRESS_FAMILY_IPV4 &&
       observed.family != AF_INET) ||
      (request.local_family == UTP_ADDRESS_FAMILY_IPV6 &&
       observed.family != AF_INET6))
    return;
  const std::string peer_id(reinterpret_cast<const char *>(request.peer_id),
                            request.peer_id_length);
  std::unordered_map<std::string, Registration>::iterator entry =
      server->registrations.find(peer_id);
  if (entry == server->registrations.end()) {
    if (!token_is_zero(request.registration_token))
      return;
    Registration registration = {};
    if (!random_token(&registration.token))
      return;
    registration.last_register_request_id = request.registration_request_id;
    registration.last_register_was_initial = true;
    entry = server->registrations.insert(std::make_pair(peer_id, registration))
                .first;
  } else {
    const bool initial_retry =
        token_is_zero(request.registration_token) &&
        entry->second.last_register_was_initial &&
        request.registration_request_id == entry->second.last_register_request_id;
    const bool update =
        !token_is_zero(request.registration_token) &&
        memcmp(request.registration_token, entry->second.token.data(),
               entry->second.token.size()) == 0;

    if (!initial_retry && !update)
      return;
    if (update) {
      entry->second.last_register_request_id = request.registration_request_id;
      entry->second.last_register_was_initial = false;
    }
  }
  update_registration(&entry->second, request, observed);
  utp_rendezvous_registered_t reply = {};
  reply.registration_request_id = request.registration_request_id;
  memcpy(reply.registration_token, entry->second.token.data(),
         entry->second.token.size());
  if (utp_rendezvous_registered_encode(body, sizeof(body), &reply,
                                       &body_length) == UTP_INTERNAL_ERROR_OK)
    (void)send_message(server, peer, peer_length,
                       UTP_RENDEZVOUS_MESSAGE_REGISTERED, body, body_length);
  fprintf(stderr, "NTRS <- Node=%s [Register] peer_id=%s\n",
          utp_ntrs_endpoint_format(&observed, observed_text,
                                   sizeof(observed_text)),
          peer_id.c_str());
}

static void handle_ping(Server *server, const sockaddr_storage &peer,
                        socklen_t peer_length,
                        const utp_ntrs_endpoint_t &observed,
                        const utp_packet_header_t &header,
                        const utp_frame_rendezvous_t &frame) {
  utp_rendezvous_ping_t ping = {};
  utp_rendezvous_pong_t pong = {};
  uint8_t body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] =
      {};

  if (utp_rendezvous_ping_decode(&ping, frame.payload,
                                 frame.payload_length) !=
      UTP_INTERNAL_ERROR_OK)
    return;
  for (std::unordered_map<std::string, Registration>::iterator entry =
           server->registrations.begin();
       entry != server->registrations.end(); ++entry) {
    if (memcmp(ping.registration_token, entry->second.token.data(),
               entry->second.token.size()) != 0)
      continue;
    if ((entry->second.local_family == UTP_ADDRESS_FAMILY_IPV4 &&
         observed.family != AF_INET) ||
        (entry->second.local_family == UTP_ADDRESS_FAMILY_IPV6 &&
         observed.family != AF_INET6))
      return;
    entry->second.endpoint = observed;
    memcpy(pong.registration_token, ping.registration_token,
           sizeof(pong.registration_token));
    pong.acknowledged_packet_number = header.packet_number;
    if (utp_rendezvous_pong_encode(body, sizeof(body), &pong) ==
        UTP_INTERNAL_ERROR_OK)
      (void)send_message(server, peer, peer_length,
                         UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
    return;
  }
}

static void handle_pong(Server *server, const utp_ntrs_endpoint_t &observed,
                        const utp_frame_rendezvous_t &frame) {
  utp_rendezvous_pong_t pong = {};

  if (utp_rendezvous_pong_decode(&pong, frame.payload,
                                 frame.payload_length) !=
      UTP_INTERNAL_ERROR_OK)
    return;
  for (std::unordered_map<std::string, PendingRendezvous>::iterator entry =
           server->pending_rendezvous.begin();
       entry != server->pending_rendezvous.end(); ++entry) {
    if (endpoints_equal(entry->second.target_endpoint, observed) &&
        memcmp(entry->second.target_token.data(), pong.registration_token,
               entry->second.target_token.size()) == 0 &&
        entry->second.forward_packet_number == pong.acknowledged_packet_number) {
      entry->second.forward_delivered = true;
      return;
    }
  }
}

static void handle_request(Server *server, const sockaddr_storage &peer,
                           socklen_t peer_length,
                           const utp_ntrs_endpoint_t &observed,
                           const utp_frame_rendezvous_t &frame) {
  utp_rendezvous_request_t request = {};
  utp_rendezvous_candidate_plan_t source_plan = {};
  utp_rendezvous_candidate_plan_t target_plan = {};
  utp_rendezvous_redirect_t redirect = {};
  utp_rendezvous_forward_t forward = {};
  utp_rendezvous_ping_t ping = {};
  uint8_t ping_body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] =
      {};
  sockaddr_storage target_socket_address = {};
  socklen_t target_socket_length = 0u;
  char source_text[INET6_ADDRSTRLEN + 8u] = {};
  char target_text[INET6_ADDRSTRLEN + 8u] = {};
  size_t forward_body_length = 0u;

  if (utp_rendezvous_request_decode(&request, frame.payload,
                                    frame.payload_length) !=
          UTP_INTERNAL_ERROR_OK ||
      (request.local_family == UTP_ADDRESS_FAMILY_IPV4 &&
       observed.family != AF_INET) ||
      (request.local_family == UTP_ADDRESS_FAMILY_IPV6 &&
       observed.family != AF_INET6))
    return;
  expire_pending_rendezvous(server, utp_ntrs_now_ms());
  const std::string rendezvous_key(
      reinterpret_cast<const char *>(request.rendezvous_id),
      sizeof(request.rendezvous_id));
  std::unordered_map<std::string, PendingRendezvous>::iterator pending =
      server->pending_rendezvous.find(rendezvous_key);
  if (pending != server->pending_rendezvous.end()) {
    if (endpoints_equal(pending->second.source_endpoint, observed))
      (void)send_message(server, peer, peer_length,
                         UTP_RENDEZVOUS_MESSAGE_REDIRECT,
                         pending->second.redirect_body.data(),
                         pending->second.redirect_body_length);
    return;
  }
  const std::string target_peer_id(
      reinterpret_cast<const char *>(request.target_peer_id),
      request.target_peer_id_length);
  std::unordered_map<std::string, Registration>::iterator target =
      server->registrations.find(target_peer_id);
  if (target == server->registrations.end() ||
      target->second.local_family != request.local_family ||
      !make_candidate_plan(&source_plan, request.local_family,
                           request.local_port, request.local_candidates,
                           request.local_candidate_count,
                           request.reported_public_endpoint, observed) ||
      !make_candidate_plan(
          &target_plan, target->second.local_family,
          target->second.local_port, target->second.local_candidates.data(),
          target->second.local_candidate_count,
          target->second.has_reported_public_endpoint
              ? &target->second.reported_public_endpoint
              : NULL,
          target->second.endpoint) ||
      !utp_ntrs_endpoint_to_sockaddr(&target->second.endpoint,
                                     &target_socket_address,
                                     &target_socket_length))
    return;

  PendingRendezvous transaction = {};
  std::array<uint8_t, UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE> punch_token = {};
  if (!random_token(&punch_token))
    return;
  memcpy(redirect.rendezvous_id, request.rendezvous_id,
         sizeof(redirect.rendezvous_id));
  memcpy(redirect.punch_token, punch_token.data(), punch_token.size());
  redirect.target_plan = target_plan;
  if (utp_rendezvous_redirect_encode(transaction.redirect_body.data(),
                                     transaction.redirect_body.size(),
                                     &redirect,
                                     &transaction.redirect_body_length) !=
      UTP_INTERNAL_ERROR_OK)
    return;
  forward.source_peer_id = request.source_peer_id;
  forward.source_peer_id_length = request.source_peer_id_length;
  memcpy(forward.rendezvous_id, request.rendezvous_id,
         sizeof(forward.rendezvous_id));
  memcpy(forward.punch_token, punch_token.data(), punch_token.size());
  forward.source_plan = source_plan;
  uint8_t forward_body[UTP_PACKET_MTU_FLOOR] = {};
  if (utp_rendezvous_forward_encode(forward_body, sizeof(forward_body),
                                    &forward, &forward_body_length) !=
      UTP_INTERNAL_ERROR_OK)
    return;
  memcpy(ping.registration_token, target->second.token.data(),
         target->second.token.size());
  if (utp_rendezvous_ping_encode(ping_body, sizeof(ping_body), &ping) !=
      UTP_INTERNAL_ERROR_OK)
    return;

  transaction.source_endpoint = observed;
  transaction.target_endpoint = target->second.endpoint;
  transaction.target_socket_address = target_socket_address;
  transaction.target_socket_length = target_socket_length;
  memcpy(transaction.target_token.data(), target->second.token.data(),
         transaction.target_token.size());
  transaction.created_at_ms = utp_ntrs_now_ms();
  transaction.forward_retry_at_ms =
      transaction.created_at_ms + k_forward_retry_initial_delay_ms;
  transaction.forward_retries_remaining = k_forward_retry_count;
  transaction.forward_retry_delay_ms = k_forward_retry_initial_delay_ms;
  if (!send_message(server, peer, peer_length,
                    UTP_RENDEZVOUS_MESSAGE_REDIRECT,
                    transaction.redirect_body.data(),
                    transaction.redirect_body_length))
    return;
  const OutgoingFrame frames[2] = {
      {ping_body, sizeof(ping_body), UTP_RENDEZVOUS_MESSAGE_PING},
      {forward_body, forward_body_length, UTP_RENDEZVOUS_MESSAGE_FORWARD}};
  if (!send_frames(server, target_socket_address, target_socket_length, frames,
                   2u, &transaction.forward_packet_number,
                   &transaction.forward_packet,
                   &transaction.forward_packet_length))
    return;
  server->pending_rendezvous.insert(std::make_pair(rendezvous_key, transaction));
  fprintf(stderr, "NTRS <- Node=%s [Request] target_peer_id=%s\n",
          utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)),
          target_peer_id.c_str());
  fprintf(stderr, "NTRS -> Node=%s [Redirect] target=%s\n",
          utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)),
          target_peer_id.c_str());
  fprintf(stderr, "NTRS -> Node=%s [Ping|Forward] source=%s\n",
          utp_ntrs_endpoint_format(&target->second.endpoint, target_text,
                                   sizeof(target_text)),
          utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)));
}

static void on_read(evutil_socket_t, short, void *user_data) {
  Server *server = static_cast<Server *>(user_data);
  for (;;) {
    uint8_t packet[UTP_PACKET_MTU_FLOOR];
    sockaddr_storage peer = {};
    socklen_t peer_length = sizeof(peer);
    const ssize_t count =
        recvfrom(server->fd, packet, sizeof(packet), 0,
                 reinterpret_cast<sockaddr *>(&peer), &peer_length);
    utp_packet_header_t header = {};
    utp_ntrs_endpoint_t observed = {};

    if (count < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        fprintf(stderr, "event=udp_receive_failed errno=%d\n", errno);
      return;
    }
    if (utp_proto_decode_header(&header, packet, static_cast<size_t>(count)) !=
            UTP_INTERNAL_ERROR_OK ||
        header.packet_number == 0u || header.reserve != 0u ||
        static_cast<size_t>(count) !=
            UTP_PACKET_HEADER_SIZE + header.payload_length ||
        !utp_ntrs_endpoint_from_sockaddr(
            &observed, reinterpret_cast<const sockaddr *>(&peer), peer_length))
      continue;
    if (header.type == UTP_PACKET_TYPE_RENDEZVOUS) {
      utp_frame_rendezvous_t frame = {};
      if (header.scid != 0u || header.dcid != 0u ||
          utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE,
                                      header.payload_length) !=
              UTP_INTERNAL_ERROR_OK ||
          header.payload_length !=
              UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frame.payload_length)
        continue;
      if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_REGISTER)
        handle_register(server, peer, peer_length, observed, frame);
      else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PING)
        handle_ping(server, peer, peer_length, observed, header, frame);
      else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PONG)
        handle_pong(server, observed, frame);
    } else if (header.type == UTP_PACKET_TYPE_INITIAL ||
               header.type == UTP_PACKET_TYPE_0RTT) {
      utp_frame_rendezvous_t frame = {};
      uint8_t frame_type = 0u;
      size_t frame_length = 0u;
      if (header.scid == 0u || header.dcid != 0u ||
          utp_frame_measure(packet + UTP_PACKET_HEADER_SIZE,
                            header.payload_length, &frame_type,
                            &frame_length) != UTP_INTERNAL_ERROR_OK ||
          frame_type != UTP_FRAME_TYPE_RENDEZVOUS ||
          utp_frame_rendezvous_decode(&frame,
                                      packet + UTP_PACKET_HEADER_SIZE,
                                      frame_length) != UTP_INTERNAL_ERROR_OK ||
          frame.message_type != UTP_RENDEZVOUS_MESSAGE_REQUEST)
        continue;
      handle_request(server, peer, peer_length, observed, frame);
    }
  }
}

static void on_signal(evutil_socket_t, short, void *user_data) {
  event_base_loopbreak(static_cast<event_base *>(user_data));
}

static void on_timer(evutil_socket_t, short, void *user_data) {
  Server *server = static_cast<Server *>(user_data);
  const uint64_t now_ms = utp_ntrs_now_ms();

  expire_pending_rendezvous(server, now_ms);
  retry_pending_rendezvous(server, now_ms);
}

static void on_write(evutil_socket_t, short, void *user_data) {
  Server *server = static_cast<Server *>(user_data);

  while (!server->output_queue.empty()) {
    const OutgoingDatagram &datagram = server->output_queue.front();
    if (sendto(server->fd, datagram.packet.data(), datagram.packet_length, 0,
               reinterpret_cast<const sockaddr *>(&datagram.peer),
               datagram.peer_length) ==
        static_cast<ssize_t>(datagram.packet_length)) {
      server->output_queue.pop_front();
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    server->output_queue.pop_front();
  }
  event_del(server->write_event);
  server->write_event_active = false;
}

int main(int argc, char **argv) {
  CLI::App cli{"NTRS rendezvous service"};
  std::string address = "0.0.0.0";
  std::string interface_name;
  uint16_t port = 24000u;
  cli.add_option("-a", address, "Bind IP");
  cli.add_option("-p", port, "Bind UDP port");
  cli.add_option("-i", interface_name, "Bind interface");
  CLI11_PARSE(cli, argc, argv);
  const std::string bind_endpoint_text = make_endpoint_text(address, port);
  utp_ntrs_endpoint_t endpoint = {};
  Server server = {};
  sockaddr_storage socket_address = {};
  socklen_t socket_length = 0u;
  if (!utp_ntrs_endpoint_parse(bind_endpoint_text.c_str(), &endpoint) ||
      !utp_ntrs_endpoint_to_sockaddr(&endpoint, &socket_address,
                                     &socket_length) ||
      (server.fd = socket(endpoint.family, SOCK_DGRAM, 0)) < 0 ||
      !utp_ntrs_socket_bind_interface(
          server.fd, interface_name.empty() ? NULL : interface_name.c_str()) ||
      bind(server.fd, reinterpret_cast<const sockaddr *>(&socket_address),
           socket_length) != 0 ||
      evutil_make_socket_nonblocking(server.fd) != 0 ||
      (server.base = event_base_new()) == NULL ||
      (server.read_event = event_new(server.base, server.fd,
                                     EV_READ | EV_PERSIST, on_read, &server)) ==
          NULL ||
      (server.timer_event = event_new(server.base, -1, EV_PERSIST, on_timer,
                                      &server)) == NULL ||
      (server.write_event = event_new(server.base, server.fd, EV_WRITE | EV_PERSIST,
                                      on_write, &server)) == NULL ||
      event_add(server.read_event, NULL) != 0)
    return 1;
  server.next_packet_number = 1u;
  utp_ntrs_app_log_init("ntrs");
  event *signal_int = evsignal_new(server.base, SIGINT, on_signal, server.base);
  event *signal_term =
      evsignal_new(server.base, SIGTERM, on_signal, server.base);
  const timeval timer_interval = {0, 100000};
  if (signal_int == NULL || signal_term == NULL ||
      event_add(signal_int, NULL) != 0 || event_add(signal_term, NULL) != 0 ||
      event_add(server.timer_event, &timer_interval) != 0)
    return 1;
  char formatted_endpoint[INET6_ADDRSTRLEN + 8u] = {};
  fprintf(stderr, "NTRS [Started] bind=%s\n",
          utp_ntrs_endpoint_format(&endpoint, formatted_endpoint,
                                   sizeof(formatted_endpoint)));
  (void)event_base_dispatch(server.base);
  event_free(signal_int);
  event_free(signal_term);
  event_free(server.write_event);
  event_free(server.timer_event);
  event_free(server.read_event);
  event_base_free(server.base);
  close(server.fd);
  return 0;
}
