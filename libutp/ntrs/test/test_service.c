#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <event2/event.h>
#include <netinet/in.h>
#include <ntrs/service.h>
#include <sys/socket.h>

#include "proto/proto.h"
#include "service_util.h"
#include "tls_stream.h"
#include "udp_probe.h"

static utp_ntrs_endpoint_t test_endpoint(uint8_t family, uint16_t port,
                                         uint8_t last_octet) {
  utp_ntrs_endpoint_t endpoint = {
      .family = family,
      .port = port,
  };

  if (family == (uint8_t)AF_INET) {
    endpoint.address[0] = 198u;
    endpoint.address[1] = 51u;
    endpoint.address[2] = 100u;
    endpoint.address[3] = last_octet;
  } else {
    endpoint.address[15] = last_octet;
  }
  return endpoint;
}

static void test_service_endpoint_resolution(void) {
  const struct sockaddr_in source = {
      .sin_family = AF_INET,
      .sin_port = htons(24001u),
      .sin_addr = {.s_addr = htonl(UINT32_C(0xc0000201))},
  };
  utp_ntrs_endpoint_t endpoint;

  assert(utp_ntrs_endpoint_resolve("127.0.0.1:24000", &endpoint));
  assert(endpoint.family == (uint8_t)AF_INET);
  assert(endpoint.port == 24000u);
  assert(utp_ntrs_endpoint_resolve("localhost:24000", &endpoint));
  assert(endpoint.port == 24000u);
  assert(endpoint.family == (uint8_t)AF_INET);
  assert(utp_ntrs_endpoint_resolve_for_family("[::1]:24000", AF_INET6,
                                               &endpoint));
  assert(endpoint.family == (uint8_t)AF_INET6);
  assert(endpoint.port == 24000u);
  assert(!utp_ntrs_endpoint_resolve_for_family("127.0.0.1:24000", AF_INET6,
                                                &endpoint));
  assert(utp_ntrs_endpoint_from_sockaddr(&endpoint,
                                         (const struct sockaddr *)&source,
                                         (socklen_t)sizeof(source)));
  assert(endpoint.family == (uint8_t)AF_INET);
  assert(endpoint.port == 24001u);
  assert(endpoint.address[0] == 192u && endpoint.address[1] == 0u &&
         endpoint.address[2] == 2u && endpoint.address[3] == 1u);
  assert(utp_ntrs_socket_bind_interface(-1, NULL));
}

static utp_ntrs_node_registration_t
test_registration(uint8_t id, uint8_t boot, uint8_t public_ip, uint32_t load) {
  utp_ntrs_node_registration_t registration = {
      .load = load,
      .heartbeat_ms = 1000u,
      .ipv4 =
          {
              .family = (uint8_t)AF_INET,
              .valid = true,
          },
  };

  registration.instance.node_id[0] = id;
  registration.instance.boot_id[0] = boot;
  registration.ipv4.public_endpoint =
      test_endpoint((uint8_t)AF_INET, 0u, public_ip);
  registration.ipv4.probe_endpoint =
      test_endpoint((uint8_t)AF_INET, (uint16_t)(4000u + id), public_ip);
  registration.ipv4.change_port_endpoint =
      test_endpoint((uint8_t)AF_INET, (uint16_t)(5000u + id), public_ip);
  registration.ipv4.control_endpoint =
      test_endpoint((uint8_t)AF_INET, (uint16_t)(6000u + id), public_ip);
  return registration;
}

static void test_control_codec(void) {
  const utp_ntrs_node_registration_t registration =
      test_registration(1u, 9u, 1u, 7u);
  utp_ntrs_node_registration_t decoded = {0};
  utp_ntrs_assignment_t assignment = {
      .family = (uint8_t)AF_INET,
      .version = 1u,
  };
  utp_ntrs_assignment_t decoded_assignment = {0};
  const utp_ntrs_node_heartbeat_t heartbeat = {
      .instance = {.node_id = {3u}, .boot_id = {4u}},
      .load = 9u,
  };
  utp_ntrs_node_heartbeat_t decoded_heartbeat = {0};
  const utp_ntrs_assignment_request_t request = {
      .instance = {.node_id = {3u}, .boot_id = {4u}},
      .failed_primary = {.node_id = {5u}, .boot_id = {6u}},
      .assignment_version = 7u,
      .family = (uint8_t)AF_INET,
      .failed_roles = UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY,
  };
  utp_ntrs_assignment_request_t decoded_request = {0};
  const utp_ntrs_node_link_hello_t hello = {
      .instance = {.node_id = {4u}, .boot_id = {5u}},
      .initiator_node_id = {6u},
      .initiator_nonce = {7u},
  };
  utp_ntrs_node_link_hello_t decoded_hello = {0};
  const utp_ntrs_forward_filter_response_t forward = {
      .target = {.node_id = {8u}, .boot_id = {9u}},
      .client =
          {
              .family = (uint8_t)AF_INET,
              .port = 45678u,
              .address = {203u, 0u, 113u, 1u},
          },
      .forward_id = UINT64_C(10),
      .packet_number = UINT64_C(11),
      .token = {12u},
      .phase = 3u,
  };
  utp_ntrs_forward_filter_response_t decoded_forward = {0};
  uint8_t message[256] = {0};
  size_t length;

  length = utp_ntrs_control_encode_registration(message, sizeof(message),
                                                &registration);
  assert(length == 208u);
  assert(utp_ntrs_control_decode_registration(message, length, &decoded));
  assert(memcmp(&registration, &decoded, sizeof(registration)) == 0);
  message[2] = 1u;
  assert(!utp_ntrs_control_decode_registration(message, length, &decoded));
  length =
      utp_ntrs_control_encode_assignment(message, sizeof(message), &assignment);
  assert(length == 160u);
  assert(
      utp_ntrs_control_decode_assignment(message, length, &decoded_assignment));
  assert(!decoded_assignment.has_primary);
  assert(!decoded_assignment.has_backup);
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 4u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 5u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 6u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 7u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 8u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 9u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 10u] = 0u;
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 11u] = 0u;
  assert(!utp_ntrs_control_decode_assignment(message, length,
                                             &decoded_assignment));
  length =
      utp_ntrs_control_encode_heartbeat(message, sizeof(message), &heartbeat);
  assert(length == 44u);
  assert(
      utp_ntrs_control_decode_heartbeat(message, length, &decoded_heartbeat));
  assert(memcmp(&heartbeat, &decoded_heartbeat, sizeof(heartbeat)) == 0);
  length = utp_ntrs_control_encode_assignment_request(message, sizeof(message),
                                                      &request);
  assert(length == 116u);
  assert(utp_ntrs_control_decode_assignment_request(message, length,
                                                    &decoded_request));
  assert(memcmp(&request, &decoded_request, sizeof(request)) == 0);
  length = utp_ntrs_control_encode_link_hello(message, sizeof(message), &hello);
  assert(length == 72u);
  assert(utp_ntrs_control_decode_link_hello(message, length, &decoded_hello));
  assert(memcmp(&hello, &decoded_hello, sizeof(hello)) == 0);
  length = utp_ntrs_control_encode_forward_filter_response(
      message, sizeof(message), &forward);
  assert(length == 91u);
  assert(utp_ntrs_control_decode_forward_filter_response(message, length,
                                                         &decoded_forward));
  assert(decoded_forward.forward_id == forward.forward_id);
  assert(decoded_forward.packet_number == forward.packet_number);
  assert(decoded_forward.phase == forward.phase);
  assert(
      utp_ntrs_node_instance_equal(&decoded_forward.target, &forward.target));
  assert(decoded_forward.client.family == forward.client.family);
  assert(decoded_forward.client.port == forward.client.port);
  assert(memcmp(decoded_forward.client.address, forward.client.address,
                sizeof(forward.client.address)) == 0);
  assert(memcmp(decoded_forward.token, forward.token, sizeof(forward.token)) ==
         0);
}

typedef struct test_control_stream_callback {
  uint32_t calls;     // 已接收完整消息数
  uint8_t last_type;  // 最近一次类型
  uint32_t last_size; // 最近一次 payload 长度
} test_control_stream_callback_t;

static void test_control_stream_message(void *user_data, uint8_t type,
                                        const uint8_t *payload,
                                        uint32_t payload_length) {
  test_control_stream_callback_t *const callback = user_data;

  (void)payload;
  ++callback->calls;
  callback->last_type = type;
  callback->last_size = payload_length;
}

static void test_control_stream(void) {
  const utp_ntrs_node_registration_t registration =
      test_registration(4u, 1u, 4u, 1u);
  test_control_stream_callback_t callback = {0};
  utp_ntrs_control_stream_t stream;
  uint8_t message[256] = {0};
  uint8_t combined[512] = {0};
  size_t length;

  length = utp_ntrs_control_encode_registration(message, sizeof(message),
                                                &registration);
  utp_ntrs_control_stream_init(&stream);
  assert(utp_ntrs_control_stream_feed(&stream, message, 3u,
                                      test_control_stream_message, &callback));
  assert(callback.calls == 0u);
  assert(utp_ntrs_control_stream_feed(&stream, message + 3u, length - 3u,
                                      test_control_stream_message, &callback));
  assert(callback.calls == 1u);
  assert(callback.last_type == UTP_NTRS_CONTROL_NODE_REGISTER);
  assert(callback.last_size == 200u);
  (void)memcpy(combined, message, length);
  (void)memcpy(combined + length, message, length);
  assert(utp_ntrs_control_stream_feed(&stream, combined, length * 2u,
                                      test_control_stream_message, &callback));
  assert(callback.calls == 3u);
  message[0] = 2u;
  utp_ntrs_control_stream_init(&stream);
  assert(!utp_ntrs_control_stream_feed(&stream, message,
                                       UTP_NTRS_CONTROL_HEADER_SIZE,
                                       test_control_stream_message, &callback));
}

static void test_registration_ok(void) {
  const utp_ntrs_registration_ok_t expected = {
      .public_endpoint = {
          .family = AF_INET,
          .address = {203u, 0u, 113u, 9u},
      },
  };
  utp_ntrs_registration_ok_t decoded = {0};
  uint8_t message[UTP_NTRS_CONTROL_HEADER_SIZE + 19u] = {0};
  size_t length;

  length = utp_ntrs_control_encode_registration_ok(message, sizeof(message),
                                                    &expected);
  assert(length == sizeof(message));
  assert(utp_ntrs_control_decode_registration_ok(message, length, &decoded));
  assert(memcmp(&expected, &decoded, sizeof(expected)) == 0);
  message[UTP_NTRS_CONTROL_HEADER_SIZE + 1u] = 1u;
  assert(!utp_ntrs_control_decode_registration_ok(message, length, &decoded));
}

typedef struct test_plain_stream_callback {
  uint8_t received[16]; // 接收的明文 TCP 数据
  size_t length;        // 接收字节数
  uint32_t ready_count; // 已触发的就绪回调数
} test_plain_stream_callback_t;

static void test_plain_stream_ready(void *user_data) {
  ++((test_plain_stream_callback_t *)user_data)->ready_count;
}

static void test_plain_stream_data(void *user_data, const uint8_t *data,
                                   size_t length) {
  test_plain_stream_callback_t *const callback = user_data;

  assert(length <= sizeof(callback->received));
  (void)memcpy(callback->received, data, length);
  callback->length = length;
}

static void test_plain_tcp_stream(void) {
  const uint8_t message[] = {1u, 2u, 3u, 4u};
  const utp_ntrs_tls_callbacks_t callbacks = {
      .ready = test_plain_stream_ready,
      .plaintext = test_plain_stream_data,
  };
  int32_t sockets[2];
  struct event_base *base;
  utp_ntrs_tls_stream_t *server;
  utp_ntrs_tls_stream_t *client;
  test_plain_stream_callback_t server_callback = {0};
  test_plain_stream_callback_t client_callback = {0};
  int32_t index;

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert((base = event_base_new()) != NULL);
  assert((server = utp_ntrs_tls_stream_new(base, sockets[0], NULL, true,
                                           &callbacks, &server_callback)) !=
         NULL);
  assert(server_callback.ready_count == 1u);
  assert((client = utp_ntrs_tls_stream_new(base, sockets[1], NULL, false,
                                           &callbacks, &client_callback)) !=
         NULL);
  assert(client_callback.ready_count == 0u);
  assert(utp_ntrs_tls_stream_connected(client));
  assert(client_callback.ready_count == 1u);
  assert(utp_ntrs_tls_stream_send(client, message, sizeof(message)));
  for (index = 0; index < 4 && server_callback.length == 0u; ++index) {
    assert(event_base_loop(base, EVLOOP_ONCE) == 0);
  }
  assert(server_callback.length == sizeof(message));
  assert(memcmp(server_callback.received, message, sizeof(message)) == 0);
  utp_ntrs_tls_stream_free(client);
  utp_ntrs_tls_stream_free(server);
  event_base_free(base);
}

static void test_hub_assignment(void) {
  utp_ntrs_hub_t hub;
  const utp_ntrs_node_registration_t node_a = test_registration(1u, 1u, 1u, 8u);
  utp_ntrs_node_registration_t node_b = test_registration(2u, 1u, 2u, 2u);
  const utp_ntrs_node_registration_t node_c = test_registration(3u, 1u, 3u, 4u);
  utp_ntrs_assignment_t assignment = {0};
  bool replaced = true;

  utp_ntrs_hub_init(&hub);
  assert(utp_ntrs_hub_register(&hub, &node_a, 100u, &replaced));
  assert(!replaced);
  assert(utp_ntrs_hub_register(&hub, &node_b, 100u, &replaced));
  assert(utp_ntrs_hub_register(&hub, &node_c, 100u, &replaced));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  assert(assignment.has_primary);
  assert(assignment.has_backup);
  assert(assignment.primary.node_id[0] == 2u);
  assert(assignment.backup.node_id[0] == 3u);
  assert(assignment.version != 0u);
  {
    const uint64_t old_version = assignment.version;

    node_b.ipv4.control_endpoint.port = 7002u;
    assert(utp_ntrs_hub_register(&hub, &node_b, 101u, NULL));
    assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                       &assignment));
    assert(assignment.version == old_version);
    assert(assignment.primary_control.port == 6002u);
  }
  assert(utp_ntrs_hub_sweep_expired(&hub, 3101u) == 3u);
  utp_ntrs_hub_destroy(&hub);
}

static void test_hub_reregistration_preserves_healthy_assignment(void) {
  utp_ntrs_hub_t hub;
  const utp_ntrs_node_registration_t node_a = test_registration(1u, 1u, 1u, 8u);
  const utp_ntrs_node_registration_t node_b = test_registration(2u, 1u, 2u, 2u);
  utp_ntrs_node_registration_t node_c = test_registration(3u, 1u, 3u, 4u);
  const utp_ntrs_node_registration_t node_d = test_registration(4u, 1u, 4u, 1u);
  utp_ntrs_assignment_t before = {0};
  utp_ntrs_assignment_t after = {0};

  utp_ntrs_hub_init(&hub);
  assert(utp_ntrs_hub_register(&hub, &node_a, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_b, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_c, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_d, 100u, NULL));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_c.instance, (uint8_t)AF_INET,
                                     &before));
  assert(before.has_primary);
  assert(before.has_backup);
  node_c.load = 100u;
  assert(utp_ntrs_hub_register(&hub, &node_c, 101u, NULL));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_c.instance, (uint8_t)AF_INET,
                                     &after));
  assert(memcmp(&before, &after, sizeof(before)) == 0);
  utp_ntrs_hub_destroy(&hub);
}

static void test_hub_replacement_and_exclusion(void) {
  utp_ntrs_hub_t hub;
  const utp_ntrs_node_registration_t node_a = test_registration(1u, 1u, 1u, 8u);
  const utp_ntrs_node_registration_t node_b = test_registration(2u, 1u, 2u, 2u);
  const utp_ntrs_node_registration_t node_c = test_registration(3u, 1u, 3u, 4u);
  utp_ntrs_node_registration_t node_b_restart = node_b;
  utp_ntrs_assignment_t assignment = {0};
  utp_ntrs_assignment_request_t request = {0};
  bool replaced = false;

  node_b_restart.instance.boot_id[0] = 2u;
  utp_ntrs_hub_init(&hub);
  assert(utp_ntrs_hub_register(&hub, &node_a, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_b, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_c, 100u, NULL));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  request = (utp_ntrs_assignment_request_t){
      .instance = node_a.instance,
      .failed_primary = assignment.primary,
      .assignment_version = assignment.version,
      .family = (uint8_t)AF_INET,
      .failed_roles = UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY,
  };
  assert(utp_ntrs_hub_request_assignment(&hub, &request, 100u, &assignment));
  assert(assignment.has_primary);
  assert(assignment.primary.node_id[0] == 3u);
  assert(utp_ntrs_hub_heartbeat(&hub, &node_a.instance, 8u, 101u));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  assert(assignment.primary.node_id[0] == 3u);
  assert(utp_ntrs_hub_heartbeat(&hub, &node_b.instance, 2u, 30000u));
  assert(utp_ntrs_hub_heartbeat(&hub, &node_c.instance, 4u, 30000u));
  assert(utp_ntrs_hub_heartbeat(&hub, &node_a.instance, 8u, 30000u));
  assert(utp_ntrs_hub_sweep_expired(&hub, 30100u) == 0u);
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  assert(assignment.primary.node_id[0] == 3u);
  assert(utp_ntrs_hub_register(&hub, &node_b_restart, 200u, &replaced));
  assert(replaced);
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  assert(assignment.primary.node_id[0] == 3u);
  assert(assignment.has_backup);
  assert(assignment.backup.boot_id[0] == 2u);
  assert(!utp_ntrs_hub_heartbeat(&hub, &node_b.instance, 1u, 201u));
  assert(utp_ntrs_hub_heartbeat(&hub, &node_b_restart.instance, 1u, 201u));
  utp_ntrs_hub_destroy(&hub);
}

static void test_hub_assignment_role_replacement(void) {
  utp_ntrs_hub_t hub;
  const utp_ntrs_node_registration_t node_a = test_registration(1u, 1u, 1u, 8u);
  const utp_ntrs_node_registration_t node_b = test_registration(2u, 1u, 2u, 2u);
  const utp_ntrs_node_registration_t node_c = test_registration(3u, 1u, 3u, 4u);
  const utp_ntrs_node_registration_t node_d = test_registration(4u, 1u, 4u, 6u);
  utp_ntrs_assignment_t assignment = {0};
  utp_ntrs_assignment_request_t request = {0};

  utp_ntrs_hub_init(&hub);
  assert(utp_ntrs_hub_register(&hub, &node_a, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_b, 100u, NULL));
  assert(utp_ntrs_hub_register(&hub, &node_c, 100u, NULL));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  request = (utp_ntrs_assignment_request_t){
      .instance = node_a.instance,
      .failed_backup = assignment.backup,
      .assignment_version = assignment.version,
      .family = (uint8_t)AF_INET,
      .failed_roles = UTP_NTRS_ASSIGNMENT_ROLE_BACKUP,
  };
  assert(utp_ntrs_hub_request_assignment(&hub, &request, 101u, &assignment));
  assert(assignment.has_primary);
  assert(assignment.primary.node_id[0] == 2u);
  assert(!assignment.has_backup);
  assert(utp_ntrs_hub_register(&hub, &node_d, 102u, NULL));
  assert(utp_ntrs_hub_get_assignment(&hub, &node_a.instance, (uint8_t)AF_INET,
                                     &assignment));
  assert(assignment.has_primary);
  assert(assignment.primary.node_id[0] == 2u);
  assert(assignment.has_backup);
  assert(assignment.backup.node_id[0] == 4u);
  request = (utp_ntrs_assignment_request_t){
      .instance = node_a.instance,
      .failed_primary = assignment.primary,
      .assignment_version = assignment.version,
      .family = (uint8_t)AF_INET,
      .failed_roles = UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY,
  };
  assert(utp_ntrs_hub_request_assignment(&hub, &request, 103u, &assignment));
  assert(assignment.has_primary);
  assert(assignment.primary.node_id[0] == 4u);
  assert(!assignment.has_backup);
  --request.assignment_version;
  assert(utp_ntrs_hub_request_assignment(&hub, &request, 104u, &assignment));
  assert(assignment.primary.node_id[0] == 4u);
  assert(!assignment.has_backup);
  utp_ntrs_hub_destroy(&hub);
}

static void test_link_deduplication(void) {
  utp_ntrs_node_link_t link = {0};
  const utp_ntrs_node_instance_t remote = {
      .node_id = {9u},
      .boot_id = {1u},
  };
  const uint8_t first_id[UTP_NTRS_NODE_ID_SIZE] = {7u};
  const uint8_t second_id[UTP_NTRS_NODE_ID_SIZE] = {2u};
  const uint8_t nonce[UTP_NTRS_LINK_NONCE_SIZE] = {4u};
  bool replaced = false;

  assert(
      utp_ntrs_node_link_consider(&link, &remote, first_id, nonce, &replaced));
  assert(!replaced);
  assert(
      utp_ntrs_node_link_consider(&link, &remote, second_id, nonce, &replaced));
  assert(replaced);
  assert(
      !utp_ntrs_node_link_consider(&link, &remote, first_id, nonce, &replaced));
}

static void test_write_u16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8u);
  data[1] = (uint8_t)value;
}

static void test_probe_request(uint8_t packet[128], uint64_t packet_number,
                               uint8_t message_type, uint8_t phase) {
  const utp_packet_header_t header = {
      .packet_number = packet_number,
      .payload_length = 108u,
      .type = 0x07u,
  };
  uint8_t *const payload = packet + UTP_PACKET_HEADER_SIZE;
  uint8_t index;

  assert(utp_proto_encode_header(packet, 128u, &header) ==
         UTP_INTERNAL_ERROR_OK);
  payload[0] = 1u;
  payload[1] = message_type;
  payload[2] = phase;
  payload[3] = 0u;
  test_write_u16(payload + 4u, 1u);
  test_write_u16(payload + 6u, 12u);
  for (index = 0u; index < 12u; ++index) {
    payload[8u + index] = index;
  }
  test_write_u16(payload + 20u, 12u);
  test_write_u16(payload + 22u, 84u);
  for (index = 24u; index < 108u; ++index) {
    payload[index] = 0u;
  }
}

static uint16_t test_response_origin_port(const uint8_t *response,
                                          size_t response_length) {
  size_t offset = UTP_PACKET_HEADER_SIZE + 4u;

  assert(response_length >= offset + 16u + 12u);
  offset += 16u;
  assert(response[offset] == 0u && response[offset + 1u] == 6u);
  offset += 4u + 8u;
  assert(response[offset] == 0u && response[offset + 1u] == 7u);
  assert(response[offset + 2u] == 0u && response[offset + 3u] == 8u);
  return (uint16_t)(((uint16_t)response[offset + 6u] << 8u) |
                    response[offset + 7u]);
}

static void test_udp_probe_handler(void) {
  const utp_ntrs_endpoint_t client =
      test_endpoint((uint8_t)AF_INET, 40000u, 10u);
  const utp_ntrs_endpoint_t probe = test_endpoint((uint8_t)AF_INET, 3478u, 20u);
  const utp_ntrs_endpoint_t change_port =
      test_endpoint((uint8_t)AF_INET, 3479u, 20u);
  const utp_ntrs_endpoint_t alternate =
      test_endpoint((uint8_t)AF_INET, 3478u, 30u);
  utp_ntrs_udp_reply_socket_t reply_socket = UTP_NTRS_UDP_REPLY_CHANGE_PORT;
  utp_packet_header_t response_header = {0};
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  size_t response_length = 0u;

  test_probe_request(request, 99u, 1u, 1u);
  assert(utp_ntrs_udp_handle_probe_request(
      request, sizeof(request), &client, &probe, &change_port, &alternate,
      response, sizeof(response), &response_length, &reply_socket));
  assert(reply_socket == UTP_NTRS_UDP_REPLY_PROBE);
  assert(utp_proto_decode_header(&response_header, response, response_length) ==
         UTP_INTERNAL_ERROR_OK);
  assert(response_header.packet_number == 99u);
  assert(response_header.type == 0x07u);
  assert(response[UTP_PACKET_HEADER_SIZE + 1u] == 2u);
  test_probe_request(request, 100u, 3u, 2u);
  assert(utp_ntrs_udp_handle_probe_request(
      request, sizeof(request), &client, &probe, &change_port, &alternate,
      response, sizeof(response), &response_length, &reply_socket));
  assert(reply_socket == UTP_NTRS_UDP_REPLY_CHANGE_PORT);
  assert(response[UTP_PACKET_HEADER_SIZE + 1u] == 4u);
  assert(test_response_origin_port(response, response_length) == 3479u);
  test_probe_request(request, 101u, 3u, 3u);
  assert(!utp_ntrs_udp_handle_probe_request(
      request, sizeof(request), &client, &probe, &change_port, &alternate,
      response, sizeof(response), &response_length, &reply_socket));
}

static uint16_t test_allocate_loopback_port(void) {
  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  socklen_t address_length = (socklen_t)sizeof(address);
  const int32_t socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

  assert(socket_fd >= 0);
  assert(bind(socket_fd, (const struct sockaddr *)&address, address_length) ==
         0);
  assert(getsockname(socket_fd, (struct sockaddr *)&address, &address_length) ==
         0);
  assert(close(socket_fd) == 0);
  return ntohs(address.sin_port);
}

static uint16_t test_allocate_ipv6_loopback_port(void) {
  struct sockaddr_in6 address = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  socklen_t address_length = (socklen_t)sizeof(address);
  const int32_t socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);

  assert(socket_fd >= 0);
  assert(bind(socket_fd, (const struct sockaddr *)&address, address_length) ==
         0);
  assert(getsockname(socket_fd, (struct sockaddr *)&address, &address_length) ==
         0);
  assert(close(socket_fd) == 0);
  return ntohs(address.sin6_port);
}

static void test_udp_worker_round_trip(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
      .worker_count = 2u,
  };
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);

  assert(server != NULL);
  assert(client_fd >= 0);
  test_probe_request(request, 333u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  assert(response[UTP_PACKET_HEADER_SIZE + 1u] == 2u);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
}

static void test_udp_worker_ipv6_round_trip(void) {
  const uint16_t probe_port = test_allocate_ipv6_loopback_port();
  const uint16_t change_port = test_allocate_ipv6_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET6, probe_port, 1u),
      .change_port_endpoint =
          test_endpoint((uint8_t)AF_INET6, change_port, 1u),
  };
  const struct sockaddr_in6 destination = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(probe_port),
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET6, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[15] = 1u;
  options.change_port_endpoint.address[15] = 1u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  assert(client_fd >= 0);
  test_probe_request(request, 337u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                (socklen_t)sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  assert(response[UTP_PACKET_HEADER_SIZE + 1u] == 2u);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
}

static void test_udp_worker_change_port(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
  };
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct sockaddr_in source;
  socklen_t source_length = (socklen_t)sizeof(source);
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  assert(client_fd >= 0);
  test_probe_request(request, 334u, 3u, 2u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0,
                  (struct sockaddr *)&source, &source_length) > 0);
  assert(ntohs(source.sin_port) == change_port);
  assert(test_response_origin_port(response, sizeof(response)) == change_port);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
}

static void test_udp_worker_source_rate_limit(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
      .source_rate_per_second = 1u,
      .source_burst = 1u,
  };
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  assert(client_fd >= 0);
  test_probe_request(request, 340u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  test_probe_request(request, 341u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  poll_fd.revents = 0;
  assert(poll(&poll_fd, 1u, 100) == 0);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
}

static void test_udp_worker_source_rate_refill(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
      .source_rate_per_second = 1u,
      .source_burst = 1u,
      .worker_count = 2u,
  };
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  assert(client_fd >= 0);
  test_probe_request(request, 342u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  assert(usleep(600000u) == 0);
  test_probe_request(request, 343u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd.revents = 0;
  assert(poll(&poll_fd, 1u, 150) == 0);
  assert(usleep(500000u) == 0);
  test_probe_request(request, 344u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd.revents = 0;
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
}

static void test_udp_worker_source_rate_shared_by_ip(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
      .source_rate_per_second = 1u,
      .source_burst = 1u,
      .worker_count = 2u,
  };
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  int32_t clients[4] = {-1, -1, -1, -1};
  struct pollfd poll_fds[4];
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  uint32_t response_count = 0u;
  uint32_t index;
  utp_ntrs_udp_server_t *server;

  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  for (index = 0u; index < 4u; ++index) {
    clients[index] = socket(AF_INET, SOCK_DGRAM, 0);
    assert(clients[index] >= 0);
    test_probe_request(request, (uint64_t)(350u + index), 1u, 1u);
    assert(sendto(clients[index], request, sizeof(request), 0,
                  (const struct sockaddr *)&destination,
                  sizeof(destination)) == (ssize_t)sizeof(request));
    poll_fds[index] = (struct pollfd){
        .fd = clients[index],
        .events = POLLIN,
    };
  }
  assert(poll(poll_fds, 4u, 1000) >= 1);
  for (index = 0u; index < 4u; ++index) {
    if ((poll_fds[index].revents & POLLIN) != 0) {
      assert(recvfrom(clients[index], response, sizeof(response), 0, NULL,
                      NULL) > 0);
      ++response_count;
    }
    assert(close(clients[index]) == 0);
  }
  assert(response_count == 1u);
  utp_ntrs_udp_server_stop(server);
}

typedef struct test_forward_capture {
  utp_ntrs_forward_filter_response_t forward; // 主 loop 收到的转发请求
  uint32_t calls;                             // 回调次数
} test_forward_capture_t;

static void
test_on_udp_forward(void *user_data,
                    const utp_ntrs_forward_filter_response_t *forward) {
  test_forward_capture_t *const capture = user_data;

  capture->forward = *forward;
  ++capture->calls;
}

static void test_udp_worker_change_ip_forward(void) {
  const uint16_t probe_port = test_allocate_loopback_port();
  const uint16_t change_port = test_allocate_loopback_port();
  const utp_ntrs_node_instance_t primary = {
      .node_id = {19u},
      .boot_id = {20u},
  };
  struct event_base *const base = event_base_new();
  test_forward_capture_t capture = {0};
  utp_ntrs_udp_server_options_t options = {
      .probe_endpoint = test_endpoint((uint8_t)AF_INET, probe_port, 1u),
      .change_port_endpoint = test_endpoint((uint8_t)AF_INET, change_port, 1u),
      .main_base = base,
      .on_forward = test_on_udp_forward,
      .user_data = &capture,
  };
  utp_ntrs_endpoint_t alternate = test_endpoint((uint8_t)AF_INET, 3478u, 2u);
  struct sockaddr_in destination = {
      .sin_family = AF_INET,
      .sin_port = htons(probe_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  struct sockaddr_in source;
  socklen_t source_length = (socklen_t)sizeof(source);
  struct pollfd poll_fd;
  uint8_t request[128] = {0};
  uint8_t response[128] = {0};
  const int32_t client_fd = socket(AF_INET, SOCK_DGRAM, 0);
  utp_ntrs_udp_server_t *server;

  assert(base != NULL);
  options.probe_endpoint.address[0] = 127u;
  options.probe_endpoint.address[1] = 0u;
  options.probe_endpoint.address[2] = 0u;
  options.change_port_endpoint.address[0] = 127u;
  options.change_port_endpoint.address[1] = 0u;
  options.change_port_endpoint.address[2] = 0u;
  alternate.address[0] = 127u;
  alternate.address[1] = 0u;
  alternate.address[2] = 0u;
  server = utp_ntrs_udp_server_start(&options);
  assert(server != NULL);
  assert(client_fd >= 0);
  utp_ntrs_udp_server_set_primary(server, &primary, &alternate);
  test_probe_request(request, 335u, 1u, 1u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0, NULL, NULL) > 0);
  test_probe_request(request, 336u, 3u, 3u);
  assert(sendto(client_fd, request, sizeof(request), 0,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == (ssize_t)sizeof(request));
  assert(event_base_loop(base, EVLOOP_ONCE) == 0);
  assert(capture.calls == 1u);
  assert(capture.forward.forward_id != 0u);
  assert(capture.forward.packet_number == 336u);
  assert(capture.forward.phase == 3u);
  assert(utp_ntrs_node_instance_equal(&capture.forward.target, &primary));
  assert(utp_ntrs_udp_server_send_filter_response(server, &capture.forward));
  poll_fd = (struct pollfd){
      .fd = client_fd,
      .events = POLLIN,
  };
  assert(poll(&poll_fd, 1u, 1000) == 1);
  assert(recvfrom(client_fd, response, sizeof(response), 0,
                  (struct sockaddr *)&source, &source_length) > 0);
  assert(ntohs(source.sin_port) == probe_port);
  assert(response[UTP_PACKET_HEADER_SIZE + 1u] == 4u);
  assert(response[UTP_PACKET_HEADER_SIZE + 2u] == 3u);
  utp_ntrs_udp_server_stop(server);
  assert(close(client_fd) == 0);
  event_base_free(base);
}

int main(void) {
  test_service_endpoint_resolution();
  test_control_codec();
  test_control_stream();
  test_registration_ok();
  test_plain_tcp_stream();
  test_hub_assignment();
  test_hub_reregistration_preserves_healthy_assignment();
  test_hub_replacement_and_exclusion();
  test_hub_assignment_role_replacement();
  test_link_deduplication();
  test_udp_probe_handler();
  test_udp_worker_round_trip();
  test_udp_worker_ipv6_round_trip();
  test_udp_worker_change_port();
  test_udp_worker_source_rate_limit();
  test_udp_worker_source_rate_refill();
  test_udp_worker_source_rate_shared_by_ip();
  test_udp_worker_change_ip_forward();
  (void)puts("utp_ntrs_service_test: ok");
  return 0;
}
