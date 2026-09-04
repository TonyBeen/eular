#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <array>
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
  uint64_t last_register_request_id;
  bool last_register_was_initial;
};

struct Server {
  event_base *base;
  event *read_event;
  int fd;
  uint64_t next_packet_number;
  std::unordered_map<std::string, Registration> registrations;
};

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

static std::string make_endpoint_text(const std::string &address,
                                      uint16_t port) {
  const std::string port_text = std::to_string(port);

  if (!address.empty() && address[0] == '[')
    return address + ":" + port_text;
  if (address.find(':') != std::string::npos)
    return "[" + address + "]:" + port_text;
  return address + ":" + port_text;
}

static bool send_message(Server *server, const sockaddr_storage &peer,
                         socklen_t peer_length, uint8_t message_type,
                         const uint8_t *body, size_t body_length) {
  uint8_t packet[UTP_PACKET_MTU_FLOOR] = {};
  utp_packet_header_t header = {};
  utp_frame_rendezvous_t frame = {body, static_cast<uint16_t>(body_length),
                                  message_type};
  const size_t packet_length =
      UTP_PACKET_HEADER_SIZE + UTP_FRAME_RENDEZVOUS_HEADER_SIZE + body_length;

  if (body_length > UINT16_MAX || packet_length > sizeof(packet) ||
      server->next_packet_number == 0u ||
      server->next_packet_number > UTP_PACKET_NUMBER_MAX)
    return false;
  header.packet_number = server->next_packet_number++;
  header.payload_length =
      static_cast<uint16_t>(UTP_FRAME_RENDEZVOUS_HEADER_SIZE + body_length);
  header.type = UTP_PACKET_TYPE_RENDEZVOUS;
  return utp_proto_encode_header(packet, sizeof(packet), &header) ==
             UTP_INTERNAL_ERROR_OK &&
         utp_frame_rendezvous_encode(packet + UTP_PACKET_HEADER_SIZE,
                                     sizeof(packet) - UTP_PACKET_HEADER_SIZE,
                                     &frame) == UTP_INTERNAL_ERROR_OK &&
         sendto(server->fd, packet, packet_length, 0,
                reinterpret_cast<const sockaddr *>(&peer),
                peer_length) == static_cast<ssize_t>(packet_length);
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
    utp_frame_rendezvous_t frame = {};
    utp_ntrs_endpoint_t observed = {};

    if (count < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        fprintf(stderr, "event=udp_receive_failed errno=%d\n", errno);
      return;
    }
    if (utp_proto_decode_header(&header, packet, static_cast<size_t>(count)) !=
            UTP_INTERNAL_ERROR_OK ||
        header.type != UTP_PACKET_TYPE_RENDEZVOUS || header.scid != 0u ||
        header.dcid != 0u || header.packet_number == 0u ||
        header.reserve != 0u ||
        static_cast<size_t>(count) !=
            UTP_PACKET_HEADER_SIZE + header.payload_length ||
        utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE,
                                    header.payload_length) !=
            UTP_INTERNAL_ERROR_OK ||
        header.payload_length !=
            UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frame.payload_length ||
        !utp_ntrs_endpoint_from_sockaddr(
            &observed, reinterpret_cast<const sockaddr *>(&peer), peer_length))
      continue;
    if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_REGISTER) {
      utp_rendezvous_register_t request = {};
      uint8_t body[64] = {};
      char observed_text[INET6_ADDRSTRLEN + 8u] = {};
      size_t body_length = 0u;
      if (utp_rendezvous_register_decode(&request, frame.payload,
                                         frame.payload_length) !=
              UTP_INTERNAL_ERROR_OK ||
          (request.local_family == 4u && observed.family != AF_INET) ||
          (request.local_family == 6u && observed.family != AF_INET6))
        continue;
      const std::string peer_id(reinterpret_cast<const char *>(request.peer_id),
                                request.peer_id_length);
      std::unordered_map<std::string, Registration>::iterator entry =
          server->registrations.find(peer_id);
      if (entry == server->registrations.end()) {
        if (!token_is_zero(request.registration_token))
          continue;
        Registration registration = {};
        if (!random_token(&registration.token))
          continue;
        registration.endpoint = observed;
        registration.last_register_request_id = request.registration_request_id;
        registration.last_register_was_initial = true;
        entry =
            server->registrations.insert(std::make_pair(peer_id, registration))
                .first;
      } else {
        const bool initial_retry = token_is_zero(request.registration_token) &&
                                   entry->second.last_register_was_initial &&
                                   request.registration_request_id ==
                                       entry->second.last_register_request_id;
        const bool update =
            !token_is_zero(request.registration_token) &&
            memcmp(request.registration_token, entry->second.token.data(),
                   entry->second.token.size()) == 0;

        if (!initial_retry && !update)
          continue;
        if (update) {
          entry->second.last_register_request_id =
              request.registration_request_id;
          entry->second.last_register_was_initial = false;
        }
      }
      entry->second.endpoint = observed;
      utp_rendezvous_registered_t reply = {};
      reply.registration_request_id = request.registration_request_id;
      memcpy(reply.registration_token, entry->second.token.data(),
             entry->second.token.size());
      if (utp_rendezvous_registered_encode(body, sizeof(body), &reply,
                                           &body_length) ==
          UTP_INTERNAL_ERROR_OK)
        (void)send_message(server, peer, peer_length,
                           UTP_RENDEZVOUS_MESSAGE_REGISTERED, body,
                           body_length);
      fprintf(stderr, "NTRS <- Node=%s [Register] peer_id=%s\n",
              utp_ntrs_endpoint_format(&observed, observed_text,
                                       sizeof(observed_text)),
              peer_id.c_str());
    } else if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PING) {
      utp_rendezvous_ping_t ping = {};
      utp_rendezvous_pong_t pong = {};
      uint8_t body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] =
          {};
      bool found = false;
      if (utp_rendezvous_ping_decode(&ping, frame.payload,
                                     frame.payload_length) !=
          UTP_INTERNAL_ERROR_OK)
        continue;
      for (std::unordered_map<std::string, Registration>::iterator entry =
               server->registrations.begin();
           entry != server->registrations.end(); ++entry) {
        if (memcmp(ping.registration_token, entry->second.token.data(),
                   entry->second.token.size()) == 0) {
          entry->second.endpoint = observed;
          memcpy(pong.registration_token, ping.registration_token,
                 sizeof(pong.registration_token));
          pong.acknowledged_packet_number = header.packet_number;
          found = true;
          break;
        }
      }
      if (found && utp_rendezvous_pong_encode(body, sizeof(body), &pong) ==
                       UTP_INTERNAL_ERROR_OK)
        (void)send_message(server, peer, peer_length,
                           UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
    }
  }
}

static void on_signal(evutil_socket_t, short, void *user_data) {
  event_base_loopbreak(static_cast<event_base *>(user_data));
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
      event_add(server.read_event, NULL) != 0)
    return 1;
  server.next_packet_number = 1u;
  utp_ntrs_app_log_init("ntrs");
  event *signal_int = evsignal_new(server.base, SIGINT, on_signal, server.base);
  event *signal_term =
      evsignal_new(server.base, SIGTERM, on_signal, server.base);
  if (signal_int == NULL || signal_term == NULL ||
      event_add(signal_int, NULL) != 0 || event_add(signal_term, NULL) != 0)
    return 1;
  char formatted_endpoint[INET6_ADDRSTRLEN + 8u] = {};
  fprintf(stderr, "NTRS [Started] bind=%s\n",
          utp_ntrs_endpoint_format(&endpoint, formatted_endpoint,
                                   sizeof(formatted_endpoint)));
  (void)event_base_dispatch(server.base);
  event_free(signal_int);
  event_free(signal_term);
  event_free(server.read_event);
  event_base_free(server.base);
  close(server.fd);
  return 0;
}
