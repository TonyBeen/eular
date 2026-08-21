#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <ntrs/service.h>
#include <openssl/sha.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include "peer_manager.h"
#include "service_util.h"
#include "tls_stream.h"

#define UTP_NTRS_FORWARD_DEDUP_CAPACITY 1024u
#define UTP_NTRS_FORWARD_DEDUP_LIFETIME_MS 30000u

typedef struct nat_detect_node_forward_dedup {
  uint64_t forward_id;    // 已处理的转发标识
  uint64_t expires_at_ms; // 去重记录失效时刻
} nat_detect_node_forward_dedup_t;

typedef struct nat_detect_node {
  struct event_base *base;                   // Node 主 libevent loop
  struct event *reconnect_event;             // Hub 断线重连定时器
  struct event *heartbeat_event;             // Hub 心跳定时器
  struct event *peer_tick_event;             // Node 间链路保活定时器
  SSL_CTX *hub_tls_context;                  // Hub 客户端 TLS 上下文，空指针表示明文 TCP
  SSL_CTX *peer_server_tls_context;          // Node 入站 TLS 上下文，空指针表示明文 TCP
  utp_ntrs_tls_stream_t *hub_stream;         // 当前 Hub 控制连接
  utp_ntrs_control_stream_t control;         // Hub 控制消息重组状态
  utp_ntrs_node_registration_t registration; // 本 Node 的固定注册信息
  utp_ntrs_endpoint_t hub_endpoint;          // Hub TCP endpoint
  const char *interface_name;                 // 指定的 Linux 出口网卡
  const char *node_name;                      // 操作员可读的 Node 名称
  utp_ntrs_udp_server_t *udp_server;         // UDP NAT 探测 worker
  utp_ntrs_peer_manager_t *peers;            // Node 间控制链路管理器
  utp_ntrs_assignment_t assignments[2];      // IPv4、IPv6 当前 assignment
  nat_detect_node_forward_dedup_t
      forward_dedup[UTP_NTRS_FORWARD_DEDUP_CAPACITY]; // 协同回包短期去重表
  uint64_t assignment_versions[2]; // IPv4、IPv6 已接受 assignment 版本
  bool registered : 1;             // Hub 已接受注册
  bool close_scheduled : 1;
} nat_detect_node_t;

static void nat_detect_node_disconnect(nat_detect_node_t *node);
static void nat_detect_node_schedule_disconnect(nat_detect_node_t *node);
static bool nat_detect_node_send(nat_detect_node_t *node,
                                 const uint8_t *message, size_t length);
static void nat_detect_node_on_udp_forward(
    void *user_data, const utp_ntrs_forward_filter_response_t *forward);
static void nat_detect_node_on_peer_forward(
    void *user_data, const utp_ntrs_forward_filter_response_t *forward);

static const char *
nat_detect_node_instance(const utp_ntrs_node_instance_t *instance,
                         char text[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE]) {
  return utp_ntrs_node_instance_format(instance, text,
                                       UTP_NTRS_NODE_INSTANCE_TEXT_SIZE);
}

static uint8_t nat_detect_node_family_slot(uint8_t family) {
  return family == (uint8_t)AF_INET ? (uint8_t)0u : (uint8_t)1u;
}

static bool nat_detect_node_send_assignment_request(nat_detect_node_t *node,
                                                    uint8_t slot,
                                                    uint8_t failed_roles) {
  const utp_ntrs_assignment_t *const assignment = &node->assignments[slot];
  utp_ntrs_assignment_request_t request = {
      .instance = node->registration.instance,
      .assignment_version = assignment->version,
      .family = assignment->family,
      .failed_roles = failed_roles,
  };
  uint8_t message[UTP_NTRS_CONTROL_HEADER_SIZE + 108u];
  size_t length;

  if (!node->registered || assignment->version == 0u || failed_roles == 0u) {
    return false;
  }
  if ((failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) != 0u) {
    request.failed_primary = assignment->primary;
  }
  if ((failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) != 0u) {
    request.failed_backup = assignment->backup;
  }
  length = utp_ntrs_control_encode_assignment_request(message, sizeof(message),
                                                      &request);
  if (length == 0u || !nat_detect_node_send(node, message, length)) {
    return false;
  }
  {
    char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(stderr,
                  "nat_detect_node event=assignment_replacement_requested "
                  "node=%s family=%u version=%llu roles=%u\n",
                  nat_detect_node_instance(&node->registration.instance, local),
                  (uint32_t)assignment->family,
                  (unsigned long long)assignment->version,
                  (uint32_t)failed_roles);
  }
  return true;
}

static void
nat_detect_node_request_replacement(nat_detect_node_t *node,
                                    const utp_ntrs_node_instance_t *remote) {
  uint8_t slot;

  for (slot = 0u; slot < 2u; ++slot) {
    const utp_ntrs_assignment_t *const assignment = &node->assignments[slot];
    uint8_t failed_roles = 0u;

    if (assignment->has_primary &&
        utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
      failed_roles |= UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY;
    }
    if (assignment->has_backup &&
        utp_ntrs_node_instance_equal(&assignment->backup, remote)) {
      failed_roles |= UTP_NTRS_ASSIGNMENT_ROLE_BACKUP;
    }
    if (failed_roles != 0u &&
        !nat_detect_node_send_assignment_request(node, slot, failed_roles)) {
      nat_detect_node_schedule_disconnect(node);
    }
  }
}

static void
nat_detect_node_on_peer_failed(void *user_data,
                               const utp_ntrs_node_instance_t *remote) {
  nat_detect_node_t *const node = user_data;
  uint8_t slot;

  {
    char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
    char peer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(stderr,
                  "nat_detect_node event=peer_link_failed node=%s peer=%s\n",
                  nat_detect_node_instance(&node->registration.instance, local),
                  nat_detect_node_instance(remote, peer));
  }
  for (slot = 0u; slot < 2u; ++slot) {
    const utp_ntrs_assignment_t *const assignment = &node->assignments[slot];

    if (assignment->has_primary &&
        utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
      utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
      (void)fprintf(
          stderr, "nat_detect_node event=alternate_probe_disabled family=%u\n",
          (uint32_t)assignment->family);
      break;
    }
  }
  nat_detect_node_request_replacement(node, remote);
}

static void
nat_detect_node_on_peer_active(void *user_data,
                               const utp_ntrs_node_instance_t *remote) {
  nat_detect_node_t *const node = user_data;
  uint8_t slot;

  for (slot = 0u; slot < 2u; ++slot) {
    const utp_ntrs_assignment_t *const assignment = &node->assignments[slot];

    if (assignment->has_primary &&
        utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
      char endpoint[64];
      char peer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

      utp_ntrs_udp_server_set_primary(node->udp_server, remote,
                                      &assignment->primary_probe);
      (void)fprintf(stderr,
                    "nat_detect_node event=primary_link_active peer=%s "
                    "family=%u alternate_probe=%s\n",
                    nat_detect_node_instance(remote, peer),
                    (uint32_t)assignment->family,
                    utp_ntrs_endpoint_format(&assignment->primary_probe,
                                             endpoint, sizeof(endpoint)));
      return;
    }
  }
}

static bool nat_detect_node_forward_seen(nat_detect_node_t *node,
                                         uint64_t forward_id) {
  const uint64_t now_ms = utp_ntrs_now_ms();
  nat_detect_node_forward_dedup_t *slot = NULL;
  uint32_t index;

  for (index = 0u; index < UTP_NTRS_FORWARD_DEDUP_CAPACITY; ++index) {
    nat_detect_node_forward_dedup_t *const current =
        &node->forward_dedup[index];

    if (current->expires_at_ms > now_ms && current->forward_id == forward_id) {
      return true;
    }
    if (current->expires_at_ms <= now_ms) {
      slot = current;
      break;
    }
    if (slot == NULL || current->expires_at_ms < slot->expires_at_ms) {
      slot = current;
    }
  }
  *slot = (nat_detect_node_forward_dedup_t){
      .forward_id = forward_id,
      .expires_at_ms = now_ms + UTP_NTRS_FORWARD_DEDUP_LIFETIME_MS,
  };
  return false;
}

static void nat_detect_node_on_udp_forward(
    void *user_data, const utp_ntrs_forward_filter_response_t *forward) {
  nat_detect_node_t *const node = user_data;

  if (!utp_ntrs_peer_manager_send_forward(node->peers, &forward->target,
                                          forward)) {
    char target[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(stderr,
                  "nat_detect_node event=filter_forward_dropped target=%s "
                  "packet_number=%llu reason=peer_unavailable\n",
                  nat_detect_node_instance(&forward->target, target),
                  (unsigned long long)forward->packet_number);
    return;
  }
  {
    char target[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(
        stderr,
        "nat_detect_node event=filter_forwarded target=%s packet_number=%llu\n",
        nat_detect_node_instance(&forward->target, target),
        (unsigned long long)forward->packet_number);
  }
}

static void nat_detect_node_on_peer_forward(
    void *user_data, const utp_ntrs_forward_filter_response_t *forward) {
  nat_detect_node_t *const node = user_data;

  if (nat_detect_node_forward_seen(node, forward->forward_id)) {
    (void)fprintf(
        stderr,
        "nat_detect_node event=filter_forward_duplicate packet_number=%llu\n",
        (unsigned long long)forward->packet_number);
    return;
  }
  if (!utp_ntrs_udp_server_send_filter_response(node->udp_server, forward)) {
    (void)fprintf(
        stderr,
        "nat_detect_node event=filter_response_failed packet_number=%llu\n",
        (unsigned long long)forward->packet_number);
    return;
  }
  (void)fprintf(
      stderr, "nat_detect_node event=filter_response_sent packet_number=%llu\n",
      (unsigned long long)forward->packet_number);
}

static bool nat_detect_node_message_from_payload(
    uint8_t type, const uint8_t *payload, uint32_t payload_length,
    uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE], size_t *length) {
  *length = utp_ntrs_control_encode_header(
      message, UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE, type, payload_length);
  if (*length == 0u) {
    return false;
  }
  (void)memcpy(message + UTP_NTRS_CONTROL_HEADER_SIZE, payload, payload_length);
  return true;
}

static void nat_detect_node_disconnect_deferred(evutil_socket_t fd,
                                                int16_t events,
                                                void *user_data) {
  nat_detect_node_t *const node = user_data;

  (void)fd;
  (void)events;
  node->close_scheduled = false;
  nat_detect_node_disconnect(node);
}

static void nat_detect_node_schedule_disconnect(nat_detect_node_t *node) {
  if (!node->close_scheduled) {
    node->close_scheduled = true;
    if (event_base_once(node->base, -1, EV_TIMEOUT,
                        nat_detect_node_disconnect_deferred, node, NULL) != 0) {
      node->close_scheduled = false;
    }
  }
}

static void nat_detect_node_disconnect(nat_detect_node_t *node) {
  if (node->hub_stream != NULL) {
    char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(
        stderr,
        "nat_detect_node event=hub_disconnected node=%s registered=%u\n",
        nat_detect_node_instance(&node->registration.instance, local),
        node->registered ? 1u : 0u);
  }
  if (node->hub_stream != NULL) {
    utp_ntrs_tls_stream_free(node->hub_stream);
    node->hub_stream = NULL;
  }
  node->registered = false;
  node->assignment_versions[0] = 0u;
  node->assignment_versions[1] = 0u;
  node->assignments[0] = (utp_ntrs_assignment_t){0};
  node->assignments[1] = (utp_ntrs_assignment_t){0};
  utp_ntrs_control_stream_init(&node->control);
  if (node->udp_server != NULL) {
    utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
  }
}

static bool nat_detect_node_send(nat_detect_node_t *node,
                                 const uint8_t *message, size_t length) {
  if (node->hub_stream == NULL ||
      !utp_ntrs_tls_stream_send(node->hub_stream, message, length)) {
    (void)fprintf(stderr, "nat_detect_node event=hub_send_failed\n");
    nat_detect_node_schedule_disconnect(node);
    return false;
  }
  return true;
}

static void nat_detect_node_on_ready(void *user_data) {
  nat_detect_node_t *const node = user_data;
  uint8_t message[UTP_NTRS_CONTROL_HEADER_SIZE + 200u];
  const size_t length = utp_ntrs_control_encode_registration(
      message, sizeof(message), &node->registration);

  if (length == 0u || !nat_detect_node_send(node, message, length)) {
    nat_detect_node_schedule_disconnect(node);
    return;
  }
  (void)fprintf(stderr,
                "nat_detect_node event=hub_control_ready registration_sent\n");
}

static void nat_detect_node_on_message(void *user_data, uint8_t type,
                                       const uint8_t *payload,
                                       uint32_t payload_length) {
  nat_detect_node_t *const node = user_data;
  uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
  size_t length;

  if (!nat_detect_node_message_from_payload(type, payload, payload_length,
                                            message, &length)) {
    (void)fprintf(stderr,
                  "nat_detect_node event=hub_message_overflow type=%u\n",
                  (uint32_t)type);
    nat_detect_node_schedule_disconnect(node);
    return;
  }
  if (!node->registered) {
    if (type != UTP_NTRS_CONTROL_NODE_REGISTER_OK || payload_length != 0u) {
      (void)fprintf(stderr,
                    "nat_detect_node event=hub_registration_rejected type=%u\n",
                    (uint32_t)type);
      nat_detect_node_schedule_disconnect(node);
      return;
    }
    node->registered = true;
    (void)fprintf(stderr, "nat_detect_node event=hub_registered\n");
    return;
  }
  if (type == UTP_NTRS_CONTROL_NODE_ASSIGNMENT) {
    utp_ntrs_assignment_t assignment;
    uint8_t slot;

    if (!utp_ntrs_control_decode_assignment(message, length, &assignment)) {
      (void)fprintf(stderr, "nat_detect_node event=assignment_invalid\n");
      nat_detect_node_schedule_disconnect(node);
      return;
    }
    if ((assignment.family == (uint8_t)AF_INET &&
         !node->registration.ipv4.valid) ||
        (assignment.family == (uint8_t)AF_INET6 &&
         !node->registration.ipv6.valid)) {
      (void)fprintf(
          stderr,
          "nat_detect_node event=assignment_family_mismatch family=%u\n",
          (uint32_t)assignment.family);
      nat_detect_node_schedule_disconnect(node);
      return;
    }
    slot = nat_detect_node_family_slot(assignment.family);
    if (assignment.version > node->assignment_versions[slot]) {
      char primary[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
      char backup[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

      node->assignment_versions[slot] = assignment.version;
      node->assignments[slot] = assignment;
      utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
      if (assignment.has_primary &&
          !utp_ntrs_peer_manager_connect(node->peers, &assignment.primary,
                                         &assignment.primary_control)) {
        (void)fprintf(
            stderr,
            "nat_detect_node event=primary_link_connect_failed family=%u\n",
            (uint32_t)assignment.family);
        nat_detect_node_request_replacement(node, &assignment.primary);
      }
      if (assignment.has_backup &&
          !utp_ntrs_peer_manager_connect(node->peers, &assignment.backup,
                                         &assignment.backup_control)) {
        (void)fprintf(
            stderr,
            "nat_detect_node event=backup_link_connect_failed family=%u\n",
            (uint32_t)assignment.family);
        nat_detect_node_request_replacement(node, &assignment.backup);
      }
      (void)fprintf(stderr,
                    "nat_detect_node event=assignment_updated family=%u "
                    "version=%llu primary=%s backup=%s\n",
                    (uint32_t)assignment.family,
                    (unsigned long long)assignment.version,
                    assignment.has_primary
                        ? nat_detect_node_instance(&assignment.primary, primary)
                        : "none",
                    assignment.has_backup
                        ? nat_detect_node_instance(&assignment.backup, backup)
                        : "none");
    }
    return;
  }
  (void)fprintf(stderr,
                "nat_detect_node event=unexpected_hub_message type=%u\n",
                (uint32_t)type);
  nat_detect_node_schedule_disconnect(node);
}

static void nat_detect_node_on_plaintext(void *user_data, const uint8_t *data,
                                         size_t length) {
  nat_detect_node_t *const node = user_data;

  if (!utp_ntrs_control_stream_feed(&node->control, data, length,
                                    nat_detect_node_on_message, node)) {
    (void)fprintf(stderr, "nat_detect_node event=invalid_hub_control_stream\n");
    nat_detect_node_schedule_disconnect(node);
  }
}

static void nat_detect_node_on_closed(void *user_data) {
  (void)fprintf(stderr, "nat_detect_node event=hub_control_closed\n");
  nat_detect_node_schedule_disconnect(user_data);
}

static void nat_detect_node_connect(nat_detect_node_t *node) {
  struct sockaddr_storage address;
  socklen_t address_length;
  int32_t fd;
  const utp_ntrs_tls_callbacks_t callbacks = {
      .ready = nat_detect_node_on_ready,
      .plaintext = nat_detect_node_on_plaintext,
      .closed = nat_detect_node_on_closed,
  };

  if (node->hub_stream != NULL ||
      !utp_ntrs_endpoint_to_sockaddr(&node->hub_endpoint, &address,
                                     &address_length)) {
    return;
  }
  fd = socket(node->hub_endpoint.family, SOCK_STREAM, 0);
  if (fd < 0 || !utp_ntrs_socket_bind_interface(fd, node->interface_name) ||
      evutil_make_socket_nonblocking(fd) != 0) {
    if (fd >= 0) {
      (void)close(fd);
    }
    (void)fprintf(stderr, "nat_detect_node event=hub_socket_create_failed\n");
    return;
  }
  {
    char endpoint[64];

    (void)fprintf(stderr, "nat_detect_node event=hub_connecting endpoint=%s\n",
                  utp_ntrs_endpoint_format(&node->hub_endpoint, endpoint,
                                           sizeof(endpoint)));
  }
  node->hub_stream = utp_ntrs_tls_stream_new(
      node->base, fd, node->hub_tls_context, false, &callbacks, node);
  if (node->hub_stream == NULL ||
      bufferevent_socket_connect(
          utp_ntrs_tls_stream_transport(node->hub_stream),
          (const struct sockaddr *)&address, (int32_t)address_length) != 0) {
    (void)fprintf(stderr, "nat_detect_node event=hub_connect_failed\n");
    nat_detect_node_disconnect(node);
  }
}

static void nat_detect_node_on_reconnect(evutil_socket_t fd, int16_t events,
                                         void *user_data) {
  (void)fd;
  (void)events;
  nat_detect_node_connect(user_data);
}

static void nat_detect_node_on_heartbeat(evutil_socket_t fd, int16_t events,
                                         void *user_data) {
  nat_detect_node_t *const node = user_data;
  const utp_ntrs_node_heartbeat_t heartbeat = {
      .instance = node->registration.instance,
      .load = node->registration.load,
  };
  uint8_t message[UTP_NTRS_CONTROL_HEADER_SIZE + 36u];
  size_t length;

  (void)fd;
  (void)events;
  if (!node->registered) {
    return;
  }
  length =
      utp_ntrs_control_encode_heartbeat(message, sizeof(message), &heartbeat);
  if (length == 0u || !nat_detect_node_send(node, message, length)) {
    (void)fprintf(stderr, "nat_detect_node event=heartbeat_send_failed\n");
    nat_detect_node_schedule_disconnect(node);
  }
}

static void nat_detect_node_on_peer_tick(evutil_socket_t fd, int16_t events,
                                         void *user_data) {
  (void)fd;
  (void)events;
  utp_ntrs_peer_manager_tick(((nat_detect_node_t *)user_data)->peers);
}

static void nat_detect_node_on_signal(evutil_socket_t fd, int16_t events,
                                      void *user_data) {
  (void)fd;
  (void)events;
  (void)fprintf(stderr, "nat_detect_node event=stopping\n");
  (void)event_base_loopbreak(user_data);
}

static bool nat_detect_node_parse_u32(const char *text, uint32_t *value) {
  char *end;
  const unsigned long parsed = strtoul(text, &end, 10);

  if (*text == '\0' || *end != '\0' || parsed > UINT32_MAX) {
    return false;
  }
  *value = (uint32_t)parsed;
  return true;
}

static void nat_detect_node_usage(const char *program) {
  (void)fprintf(stderr,
                "Usage: %s --hub HOST:PORT --node-id NAME "
                "[--probe IP:PORT] [--change-port IP:PORT] "
                "[--control IP:PORT] [--interface NAME] "
                "[--cert FILE --key FILE] [--boot-id HEX32] [--load N] "
                "[--heartbeat-ms N] [--workers N] [--source-rate N] "
                "[--source-burst N]\n",
                program);
}

int main(int argc, char **argv) {
  nat_detect_node_t node = {0};
  utp_ntrs_udp_server_options_t udp_options = {0};
  struct event *signal_int = NULL;
  struct event *signal_term = NULL;
  struct timeval reconnect_period = {.tv_sec = 1, .tv_usec = 0};
  struct timeval heartbeat_period;
  const char *node_id = NULL;
  const char *boot_id = NULL;
  const char *hub = NULL;
  const char *probe = "0.0.0.0:24001";
  const char *change_port = "0.0.0.0:24002";
  const char *control = "0.0.0.0:24003";
  const char *interface_name = NULL;
  const char *certificate = NULL;
  const char *private_key = NULL;
  uint32_t workers = 1u;
  uint32_t source_rate = 0u;
  uint32_t source_burst = 0u;
  int32_t index;
  int32_t result = EXIT_FAILURE;

  node.registration.heartbeat_ms = 5000u;
  for (index = 1; index + 1 < argc; index += 2) {
    if (strcmp(argv[index], "--hub") == 0) {
      hub = argv[index + 1];
    } else if (strcmp(argv[index], "--node-id") == 0) {
      node_id = argv[index + 1];
    } else if (strcmp(argv[index], "--boot-id") == 0) {
      boot_id = argv[index + 1];
    } else if (strcmp(argv[index], "--probe") == 0) {
      probe = argv[index + 1];
    } else if (strcmp(argv[index], "--change-port") == 0) {
      change_port = argv[index + 1];
    } else if (strcmp(argv[index], "--control") == 0) {
      control = argv[index + 1];
    } else if (strcmp(argv[index], "--interface") == 0) {
      interface_name = argv[index + 1];
    } else if (strcmp(argv[index], "--cert") == 0) {
      certificate = argv[index + 1];
    } else if (strcmp(argv[index], "--key") == 0) {
      private_key = argv[index + 1];
    } else if (strcmp(argv[index], "--load") == 0) {
      if (!nat_detect_node_parse_u32(argv[index + 1],
                                     &node.registration.load)) {
        nat_detect_node_usage(argv[0]);
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[index], "--heartbeat-ms") == 0) {
      if (!nat_detect_node_parse_u32(argv[index + 1],
                                     &node.registration.heartbeat_ms) ||
          node.registration.heartbeat_ms == 0u) {
        nat_detect_node_usage(argv[0]);
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[index], "--workers") == 0) {
      if (!nat_detect_node_parse_u32(argv[index + 1], &workers) ||
          workers == 0u || workers > UINT16_MAX) {
        nat_detect_node_usage(argv[0]);
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[index], "--source-rate") == 0) {
      if (!nat_detect_node_parse_u32(argv[index + 1], &source_rate) ||
          source_rate == 0u) {
        nat_detect_node_usage(argv[0]);
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[index], "--source-burst") == 0) {
      if (!nat_detect_node_parse_u32(argv[index + 1], &source_burst) ||
          source_burst == 0u) {
        nat_detect_node_usage(argv[0]);
        return EXIT_FAILURE;
      }
    } else {
      nat_detect_node_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (index != argc || hub == NULL || node_id == NULL || node_id[0] == '\0' ||
      ((certificate == NULL) != (private_key == NULL)) ||
      (boot_id != NULL &&
       !utp_ntrs_hex_decode(boot_id, node.registration.instance.boot_id,
                            UTP_NTRS_BOOT_ID_SIZE)) ||
      (boot_id == NULL &&
       getrandom(node.registration.instance.boot_id, UTP_NTRS_BOOT_ID_SIZE,
                 0u) != (ssize_t)UTP_NTRS_BOOT_ID_SIZE) ||
      !utp_ntrs_endpoint_resolve(hub, &node.hub_endpoint) ||
      !utp_ntrs_endpoint_parse(probe, &udp_options.probe_endpoint) ||
      !utp_ntrs_endpoint_parse(change_port,
                               &udp_options.change_port_endpoint) ||
      !utp_ntrs_endpoint_parse(control,
                               &node.registration.ipv4.control_endpoint) ||
      node.hub_endpoint.family != udp_options.probe_endpoint.family ||
      udp_options.probe_endpoint.family !=
          udp_options.change_port_endpoint.family ||
      udp_options.probe_endpoint.family !=
          node.registration.ipv4.control_endpoint.family) {
    nat_detect_node_usage(argv[0]);
    return EXIT_FAILURE;
  }
  {
    uint8_t digest[SHA256_DIGEST_LENGTH];

    if (SHA256((const uint8_t *)node_id, strlen(node_id), digest) == NULL) {
      return EXIT_FAILURE;
    }
    (void)memcpy(node.registration.instance.node_id, digest,
                 UTP_NTRS_NODE_ID_SIZE);
  }
  node.interface_name = interface_name;
  node.node_name = node_id;
  udp_options.worker_count = (uint16_t)workers;
  udp_options.source_rate_per_second = source_rate;
  udp_options.source_burst = source_burst;
  udp_options.interface_name = interface_name;
  node.registration.ipv4 = (utp_ntrs_node_family_t){
      .public_endpoint = udp_options.probe_endpoint,
      .probe_endpoint = udp_options.probe_endpoint,
      .change_port_endpoint = udp_options.change_port_endpoint,
      .control_endpoint = node.registration.ipv4.control_endpoint,
      .family = udp_options.probe_endpoint.family,
      .valid = true,
  };
  node.registration.ipv4.public_endpoint.port = 0u;
  if (node.registration.ipv4.family != (uint8_t)AF_INET) {
    node.registration.ipv6 = node.registration.ipv4;
    node.registration.ipv6.family = (uint8_t)AF_INET6;
    node.registration.ipv4 = (utp_ntrs_node_family_t){0};
  }
  if ((node.base = event_base_new()) == NULL) {
    goto cleanup;
  }
  if (certificate != NULL &&
      ((node.hub_tls_context = utp_ntrs_tls_client_context_new()) == NULL ||
       (node.peer_server_tls_context =
            utp_ntrs_tls_server_context_new(certificate, private_key)) == NULL)) {
    goto cleanup;
  }
  udp_options.main_base = node.base;
  udp_options.on_forward = nat_detect_node_on_udp_forward;
  udp_options.user_data = &node;
  if ((node.udp_server = utp_ntrs_udp_server_start(&udp_options)) == NULL ||
      (node.peers =
           utp_ntrs_peer_manager_start(&(utp_ntrs_peer_manager_options_t){
               .base = node.base,
               .client_tls = node.hub_tls_context,
               .server_tls = node.peer_server_tls_context,
               .local = node.registration.instance,
               .listen = node.registration.ipv4.valid
                             ? node.registration.ipv4.control_endpoint
                             : node.registration.ipv6.control_endpoint,
               .interface_name = node.interface_name,
               .on_active = nat_detect_node_on_peer_active,
               .on_failed = nat_detect_node_on_peer_failed,
               .on_forward = nat_detect_node_on_peer_forward,
               .user_data = &node,
           })) == NULL) {
    goto cleanup;
  }
  {
    char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
    char hub_endpoint[64];
    char probe_endpoint[64];
    char control_endpoint[64];
    const utp_ntrs_node_family_t *const family = node.registration.ipv4.valid
                                                     ? &node.registration.ipv4
                                                     : &node.registration.ipv6;

    (void)fprintf(
        stderr,
        "nat_detect_node event=starting node_name=%s node=%s hub=%s probe=%s change_port=%u "
        "control=%s workers=%u transport=%s interface=%s\n",
        node.node_name, nat_detect_node_instance(&node.registration.instance, local),
        utp_ntrs_endpoint_format(&node.hub_endpoint, hub_endpoint,
                                 sizeof(hub_endpoint)),
        utp_ntrs_endpoint_format(&family->probe_endpoint, probe_endpoint,
                                 sizeof(probe_endpoint)),
        (uint32_t)family->change_port_endpoint.port,
        utp_ntrs_endpoint_format(&family->control_endpoint, control_endpoint,
                                 sizeof(control_endpoint)),
        (uint32_t)workers, node.hub_tls_context != NULL ? "tls" : "tcp",
        node.interface_name != NULL ? node.interface_name : "any");
  }
  utp_ntrs_control_stream_init(&node.control);
  heartbeat_period.tv_sec = (int32_t)(node.registration.heartbeat_ms / 1000u);
  heartbeat_period.tv_usec =
      (int32_t)((node.registration.heartbeat_ms % 1000u) * 1000u);
  node.reconnect_event =
      event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_reconnect, &node);
  node.heartbeat_event =
      event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_heartbeat, &node);
  node.peer_tick_event =
      event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_peer_tick, &node);
  signal_int =
      evsignal_new(node.base, SIGINT, nat_detect_node_on_signal, node.base);
  signal_term =
      evsignal_new(node.base, SIGTERM, nat_detect_node_on_signal, node.base);
  if (node.reconnect_event == NULL || node.heartbeat_event == NULL ||
      node.peer_tick_event == NULL || signal_int == NULL ||
      signal_term == NULL ||
      event_add(node.reconnect_event, &reconnect_period) != 0 ||
      event_add(node.heartbeat_event, &heartbeat_period) != 0 ||
      event_add(signal_int, NULL) != 0 || event_add(signal_term, NULL) != 0 ||
      event_add(node.peer_tick_event,
                &(const struct timeval){.tv_sec = 10, .tv_usec = 0}) != 0) {
    goto cleanup;
  }
  nat_detect_node_connect(&node);
  (void)event_base_dispatch(node.base);
  result = EXIT_SUCCESS;

cleanup:
  if (signal_int != NULL) {
    event_free(signal_int);
  }
  if (signal_term != NULL) {
    event_free(signal_term);
  }
  if (node.reconnect_event != NULL) {
    event_free(node.reconnect_event);
  }
  if (node.heartbeat_event != NULL) {
    event_free(node.heartbeat_event);
  }
  if (node.peer_tick_event != NULL) {
    event_free(node.peer_tick_event);
  }
  nat_detect_node_disconnect(&node);
  if (node.peers != NULL) {
    utp_ntrs_peer_manager_stop(node.peers);
  }
  if (node.udp_server != NULL) {
    utp_ntrs_udp_server_stop(node.udp_server);
  }
  if (node.peer_server_tls_context != NULL) {
    utp_ntrs_tls_context_free(node.peer_server_tls_context);
  }
  if (node.hub_tls_context != NULL) {
    utp_ntrs_tls_context_free(node.hub_tls_context);
  }
  if (node.base != NULL) {
    event_base_free(node.base);
  }
  return result;
}
