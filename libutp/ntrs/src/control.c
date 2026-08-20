#include <string.h>

#include <netinet/in.h>
#include <ntrs/service.h>

#define UTP_NTRS_ENDPOINT_WIRE_SIZE 19u
#define UTP_NTRS_FAMILY_WIRE_SIZE 80u
#define UTP_NTRS_REGISTRATION_PAYLOAD_SIZE 200u
#define UTP_NTRS_ASSIGNMENT_PAYLOAD_SIZE 152u
#define UTP_NTRS_HEARTBEAT_PAYLOAD_SIZE 36u
#define UTP_NTRS_ASSIGNMENT_REQUEST_PAYLOAD_SIZE 108u
#define UTP_NTRS_LINK_HELLO_PAYLOAD_SIZE 64u
#define UTP_NTRS_FORWARD_FILTER_RESPONSE_PAYLOAD_SIZE 83u
#define UTP_NTRS_NAT_PHASE_CHANGE_IP 3u

static void utp_ntrs_write_u16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8u);
  data[1] = (uint8_t)value;
}

static void utp_ntrs_write_u32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24u);
  data[1] = (uint8_t)(value >> 16u);
  data[2] = (uint8_t)(value >> 8u);
  data[3] = (uint8_t)value;
}

static void utp_ntrs_write_u64(uint8_t *data, uint64_t value) {
  data[0] = (uint8_t)(value >> 56u);
  data[1] = (uint8_t)(value >> 48u);
  data[2] = (uint8_t)(value >> 40u);
  data[3] = (uint8_t)(value >> 32u);
  data[4] = (uint8_t)(value >> 24u);
  data[5] = (uint8_t)(value >> 16u);
  data[6] = (uint8_t)(value >> 8u);
  data[7] = (uint8_t)value;
}

static uint16_t utp_ntrs_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8u) | (uint16_t)data[1]);
}

static uint32_t utp_ntrs_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static uint64_t utp_ntrs_read_u64(const uint8_t *data) {
  return ((uint64_t)data[0] << 56u) | ((uint64_t)data[1] << 48u) |
         ((uint64_t)data[2] << 40u) | ((uint64_t)data[3] << 32u) |
         ((uint64_t)data[4] << 24u) | ((uint64_t)data[5] << 16u) |
         ((uint64_t)data[6] << 8u) | (uint64_t)data[7];
}

static bool utp_ntrs_control_type_valid(uint8_t type) {
  return type >= UTP_NTRS_CONTROL_NODE_REGISTER &&
         type <= UTP_NTRS_CONTROL_NAT_FORWARD_FILTER_RSP;
}

static bool utp_ntrs_family_valid(uint8_t family) {
  return family == (uint8_t)AF_INET || family == (uint8_t)AF_INET6;
}

static bool utp_ntrs_bytes_all_zero(const uint8_t *data, size_t length) {
  size_t index;

  for (index = 0u; index < length; ++index) {
    if (data[index] != 0u) {
      return false;
    }
  }
  return true;
}

static bool utp_ntrs_bytes_nonzero(const uint8_t *data, size_t length) {
  return !utp_ntrs_bytes_all_zero(data, length);
}

static bool
utp_ntrs_node_instance_valid(const utp_ntrs_node_instance_t *instance) {
  return utp_ntrs_bytes_nonzero(instance->node_id, sizeof(instance->node_id)) &&
         utp_ntrs_bytes_nonzero(instance->boot_id, sizeof(instance->boot_id));
}

static void utp_ntrs_encode_endpoint(uint8_t *data,
                                     const utp_ntrs_endpoint_t *endpoint) {
  data[0] = endpoint->family;
  utp_ntrs_write_u16(data + 1u, endpoint->port);
  (void)memcpy(data + 3u, endpoint->address, sizeof(endpoint->address));
}

static bool utp_ntrs_decode_endpoint(const uint8_t *data,
                                     utp_ntrs_endpoint_t *endpoint) {
  endpoint->family = data[0];
  endpoint->port = utp_ntrs_read_u16(data + 1u);
  (void)memcpy(endpoint->address, data + 3u, sizeof(endpoint->address));
  return utp_ntrs_family_valid(endpoint->family) &&
         (endpoint->family != (uint8_t)AF_INET ||
          utp_ntrs_bytes_all_zero(endpoint->address + 4u, 12u));
}

static void utp_ntrs_encode_family(uint8_t *data,
                                   const utp_ntrs_node_family_t *family) {
  data[0] = family->valid ? 1u : 0u;
  data[1] = family->family;
  data[2] = 0u;
  data[3] = 0u;
  utp_ntrs_encode_endpoint(data + 4u, &family->public_endpoint);
  utp_ntrs_encode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE,
                           &family->probe_endpoint);
  utp_ntrs_encode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE * 2u,
                           &family->change_port_endpoint);
  utp_ntrs_encode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE * 3u,
                           &family->control_endpoint);
}

static bool utp_ntrs_decode_family(const uint8_t *data,
                                   utp_ntrs_node_family_t *family) {
  *family = (utp_ntrs_node_family_t){0};
  if (data[0] > 1u || data[2] != 0u || data[3] != 0u) {
    return false;
  }
  if (data[0] == 0u) {
    return utp_ntrs_bytes_all_zero(data + 1u, UTP_NTRS_FAMILY_WIRE_SIZE - 1u);
  }
  family->valid = data[0] != 0u;
  family->family = data[1];
  if (!utp_ntrs_decode_endpoint(data + 4u, &family->public_endpoint) ||
      !utp_ntrs_decode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE,
                                &family->probe_endpoint) ||
      !utp_ntrs_decode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE * 2u,
                                &family->change_port_endpoint) ||
      !utp_ntrs_decode_endpoint(data + 4u + UTP_NTRS_ENDPOINT_WIRE_SIZE * 3u,
                                &family->control_endpoint)) {
    return false;
  }
  return utp_ntrs_family_valid(family->family);
}

static void utp_ntrs_encode_assignment_peer(
    uint8_t *data, const utp_ntrs_node_instance_t *instance,
    const utp_ntrs_endpoint_t *probe, const utp_ntrs_endpoint_t *control) {
  (void)memcpy(data, instance, sizeof(*instance));
  utp_ntrs_encode_endpoint(data + sizeof(*instance), probe);
  utp_ntrs_encode_endpoint(
      data + sizeof(*instance) + UTP_NTRS_ENDPOINT_WIRE_SIZE, control);
}

static bool utp_ntrs_decode_assignment_peer(const uint8_t *data,
                                            utp_ntrs_node_instance_t *instance,
                                            utp_ntrs_endpoint_t *probe,
                                            utp_ntrs_endpoint_t *control) {
  (void)memcpy(instance, data, sizeof(*instance));
  return utp_ntrs_decode_endpoint(data + sizeof(*instance), probe) &&
         utp_ntrs_decode_endpoint(
             data + sizeof(*instance) + UTP_NTRS_ENDPOINT_WIRE_SIZE, control);
}

static bool utp_ntrs_assignment_peer_valid(
    const utp_ntrs_node_instance_t *instance, const utp_ntrs_endpoint_t *probe,
    const utp_ntrs_endpoint_t *control, uint8_t family) {
  return utp_ntrs_node_instance_valid(instance) && probe->family == family &&
         control->family == family && probe->port != 0u && control->port != 0u;
}

bool utp_ntrs_control_decode_header(const uint8_t *data, size_t length,
                                    utp_ntrs_control_header_t *header) {
  uint32_t payload_length;

  if (length < UTP_NTRS_CONTROL_HEADER_SIZE ||
      data[0] != UTP_NTRS_CONTROL_VERSION ||
      !utp_ntrs_control_type_valid(data[1]) || data[2] != 0u || data[3] != 0u) {
    return false;
  }
  payload_length = utp_ntrs_read_u32(data + 4u);
  if (payload_length >
          UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE - UTP_NTRS_CONTROL_HEADER_SIZE ||
      length != UTP_NTRS_CONTROL_HEADER_SIZE + (size_t)payload_length) {
    return false;
  }
  header->type = data[1];
  header->payload_length = payload_length;
  return true;
}

size_t utp_ntrs_control_encode_header(uint8_t *data, size_t capacity,
                                      uint8_t type, uint32_t payload_length) {
  if (!utp_ntrs_control_type_valid(type) ||
      payload_length >
          UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE - UTP_NTRS_CONTROL_HEADER_SIZE ||
      capacity < UTP_NTRS_CONTROL_HEADER_SIZE + (size_t)payload_length) {
    return 0u;
  }
  data[0] = UTP_NTRS_CONTROL_VERSION;
  data[1] = type;
  data[2] = 0u;
  data[3] = 0u;
  utp_ntrs_write_u32(data + 4u, payload_length);
  return UTP_NTRS_CONTROL_HEADER_SIZE + (size_t)payload_length;
}

size_t utp_ntrs_control_encode_registration(
    uint8_t *data, size_t capacity,
    const utp_ntrs_node_registration_t *registration) {
  uint8_t *payload;

  if (utp_ntrs_control_encode_header(
          data, capacity, UTP_NTRS_CONTROL_NODE_REGISTER,
          UTP_NTRS_REGISTRATION_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  (void)memcpy(payload, &registration->instance,
               sizeof(registration->instance));
  utp_ntrs_write_u32(payload + sizeof(registration->instance),
                     registration->load);
  utp_ntrs_write_u32(payload + sizeof(registration->instance) + 4u,
                     registration->heartbeat_ms);
  utp_ntrs_encode_family(payload + 40u, &registration->ipv4);
  utp_ntrs_encode_family(payload + 40u + UTP_NTRS_FAMILY_WIRE_SIZE,
                         &registration->ipv6);
  return UTP_NTRS_CONTROL_HEADER_SIZE + UTP_NTRS_REGISTRATION_PAYLOAD_SIZE;
}

bool utp_ntrs_control_decode_registration(
    const uint8_t *data, size_t length,
    utp_ntrs_node_registration_t *registration) {
  utp_ntrs_control_header_t header;
  const uint8_t *payload;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NODE_REGISTER ||
      header.payload_length != UTP_NTRS_REGISTRATION_PAYLOAD_SIZE) {
    return false;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  *registration = (utp_ntrs_node_registration_t){0};
  (void)memcpy(&registration->instance, payload,
               sizeof(registration->instance));
  registration->load =
      utp_ntrs_read_u32(payload + sizeof(registration->instance));
  registration->heartbeat_ms =
      utp_ntrs_read_u32(payload + sizeof(registration->instance) + 4u);
  return utp_ntrs_node_instance_valid(&registration->instance) &&
         registration->heartbeat_ms != 0u &&
         utp_ntrs_decode_family(payload + 40u, &registration->ipv4) &&
         utp_ntrs_decode_family(payload + 40u + UTP_NTRS_FAMILY_WIRE_SIZE,
                                &registration->ipv6) &&
         (registration->ipv4.valid || registration->ipv6.valid);
}

size_t
utp_ntrs_control_encode_assignment(uint8_t *data, size_t capacity,
                                   const utp_ntrs_assignment_t *assignment) {
  uint8_t *payload;

  if (utp_ntrs_control_encode_header(data, capacity,
                                     UTP_NTRS_CONTROL_NODE_ASSIGNMENT,
                                     UTP_NTRS_ASSIGNMENT_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  payload[0] = assignment->family;
  payload[1] = (uint8_t)((assignment->has_primary ? 1u : 0u) |
                         (assignment->has_backup ? 2u : 0u));
  payload[2] = 0u;
  payload[3] = 0u;
  utp_ntrs_write_u64(payload + 4u, assignment->version);
  utp_ntrs_encode_assignment_peer(payload + 12u, &assignment->primary,
                                  &assignment->primary_probe,
                                  &assignment->primary_control);
  utp_ntrs_encode_assignment_peer(payload + 82u, &assignment->backup,
                                  &assignment->backup_probe,
                                  &assignment->backup_control);
  return UTP_NTRS_CONTROL_HEADER_SIZE + UTP_NTRS_ASSIGNMENT_PAYLOAD_SIZE;
}

bool utp_ntrs_control_decode_assignment(const uint8_t *data, size_t length,
                                        utp_ntrs_assignment_t *assignment) {
  utp_ntrs_control_header_t header;
  const uint8_t *payload;
  uint8_t flags;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NODE_ASSIGNMENT ||
      header.payload_length != UTP_NTRS_ASSIGNMENT_PAYLOAD_SIZE) {
    return false;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  flags = payload[1];
  if (!utp_ntrs_family_valid(payload[0]) || (flags & ~3u) != 0u ||
      payload[2] != 0u || payload[3] != 0u ||
      utp_ntrs_read_u64(payload + 4u) == 0u) {
    return false;
  }
  *assignment = (utp_ntrs_assignment_t){0};
  assignment->family = payload[0];
  assignment->has_primary = (flags & 1u) != 0u;
  assignment->has_backup = (flags & 2u) != 0u;
  assignment->version = utp_ntrs_read_u64(payload + 4u);
  if (assignment->has_primary) {
    if (!utp_ntrs_decode_assignment_peer(payload + 12u, &assignment->primary,
                                         &assignment->primary_probe,
                                         &assignment->primary_control) ||
        !utp_ntrs_assignment_peer_valid(
            &assignment->primary, &assignment->primary_probe,
            &assignment->primary_control, assignment->family)) {
      return false;
    }
  } else if (!utp_ntrs_bytes_all_zero(payload + 12u, 70u)) {
    return false;
  }
  if (assignment->has_backup) {
    return utp_ntrs_decode_assignment_peer(payload + 82u, &assignment->backup,
                                           &assignment->backup_probe,
                                           &assignment->backup_control) &&
           utp_ntrs_assignment_peer_valid(
               &assignment->backup, &assignment->backup_probe,
               &assignment->backup_control, assignment->family);
  }
  return utp_ntrs_bytes_all_zero(payload + 82u, 70u);
}

size_t
utp_ntrs_control_encode_heartbeat(uint8_t *data, size_t capacity,
                                  const utp_ntrs_node_heartbeat_t *heartbeat) {
  const size_t message_length =
      UTP_NTRS_CONTROL_HEADER_SIZE + UTP_NTRS_HEARTBEAT_PAYLOAD_SIZE;

  if (capacity < message_length ||
      utp_ntrs_control_encode_header(data, capacity,
                                     UTP_NTRS_CONTROL_NODE_HEARTBEAT,
                                     UTP_NTRS_HEARTBEAT_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  (void)memcpy(data + UTP_NTRS_CONTROL_HEADER_SIZE, &heartbeat->instance,
               sizeof(heartbeat->instance));
  utp_ntrs_write_u32(data + UTP_NTRS_CONTROL_HEADER_SIZE +
                         sizeof(heartbeat->instance),
                     heartbeat->load);
  return message_length;
}

bool utp_ntrs_control_decode_heartbeat(const uint8_t *data, size_t length,
                                       utp_ntrs_node_heartbeat_t *heartbeat) {
  utp_ntrs_control_header_t header;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NODE_HEARTBEAT ||
      header.payload_length != UTP_NTRS_HEARTBEAT_PAYLOAD_SIZE) {
    return false;
  }
  data += UTP_NTRS_CONTROL_HEADER_SIZE;
  (void)memcpy(&heartbeat->instance, data, sizeof(heartbeat->instance));
  heartbeat->load = utp_ntrs_read_u32(data + sizeof(heartbeat->instance));
  return utp_ntrs_node_instance_valid(&heartbeat->instance);
}

size_t utp_ntrs_control_encode_assignment_request(
    uint8_t *data, size_t capacity,
    const utp_ntrs_assignment_request_t *request) {
  uint8_t *payload;

  if ((request->failed_roles & ~(UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY |
                                 UTP_NTRS_ASSIGNMENT_ROLE_BACKUP)) != 0u ||
      request->failed_roles == 0u || request->assignment_version == 0u ||
      !utp_ntrs_family_valid(request->family) ||
      ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) == 0u &&
       !utp_ntrs_bytes_all_zero((const uint8_t *)&request->failed_primary,
                                sizeof(request->failed_primary))) ||
      ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) == 0u &&
       !utp_ntrs_bytes_all_zero((const uint8_t *)&request->failed_backup,
                                sizeof(request->failed_backup))) ||
      capacity < UTP_NTRS_CONTROL_HEADER_SIZE +
                     UTP_NTRS_ASSIGNMENT_REQUEST_PAYLOAD_SIZE ||
      utp_ntrs_control_encode_header(
          data, capacity, UTP_NTRS_CONTROL_NODE_ASSIGNMENT_REQUEST,
          UTP_NTRS_ASSIGNMENT_REQUEST_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  payload[0] = request->family;
  payload[1] = request->failed_roles;
  payload[2] = 0u;
  payload[3] = 0u;
  utp_ntrs_write_u64(payload + 4u, request->assignment_version);
  (void)memcpy(payload + 12u, &request->instance, sizeof(request->instance));
  (void)memcpy(payload + 44u, &request->failed_primary,
               sizeof(request->failed_primary));
  (void)memcpy(payload + 76u, &request->failed_backup,
               sizeof(request->failed_backup));
  return UTP_NTRS_CONTROL_HEADER_SIZE +
         UTP_NTRS_ASSIGNMENT_REQUEST_PAYLOAD_SIZE;
}

bool utp_ntrs_control_decode_assignment_request(
    const uint8_t *data, size_t length,
    utp_ntrs_assignment_request_t *request) {
  utp_ntrs_control_header_t header;
  const uint8_t *payload;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NODE_ASSIGNMENT_REQUEST ||
      header.payload_length != UTP_NTRS_ASSIGNMENT_REQUEST_PAYLOAD_SIZE) {
    return false;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  if (!utp_ntrs_family_valid(payload[0]) || payload[1] == 0u ||
      utp_ntrs_read_u64(payload + 4u) == 0u ||
      (payload[1] & ~(UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY |
                      UTP_NTRS_ASSIGNMENT_ROLE_BACKUP)) != 0u ||
      payload[2] != 0u || payload[3] != 0u ||
      ((payload[1] & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) == 0u &&
       !utp_ntrs_bytes_all_zero(payload + 44u,
                                sizeof(request->failed_primary))) ||
      ((payload[1] & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) == 0u &&
       !utp_ntrs_bytes_all_zero(payload + 76u,
                                sizeof(request->failed_backup)))) {
    return false;
  }
  *request = (utp_ntrs_assignment_request_t){0};
  request->family = payload[0];
  request->failed_roles = payload[1];
  request->assignment_version = utp_ntrs_read_u64(payload + 4u);
  (void)memcpy(&request->instance, payload + 12u, sizeof(request->instance));
  (void)memcpy(&request->failed_primary, payload + 44u,
               sizeof(request->failed_primary));
  (void)memcpy(&request->failed_backup, payload + 76u,
               sizeof(request->failed_backup));
  return utp_ntrs_node_instance_valid(&request->instance) &&
         ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) == 0u ||
          utp_ntrs_node_instance_valid(&request->failed_primary)) &&
         ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) == 0u ||
          utp_ntrs_node_instance_valid(&request->failed_backup));
}

size_t
utp_ntrs_control_encode_link_hello(uint8_t *data, size_t capacity,
                                   const utp_ntrs_node_link_hello_t *hello) {
  const size_t message_length =
      UTP_NTRS_CONTROL_HEADER_SIZE + UTP_NTRS_LINK_HELLO_PAYLOAD_SIZE;

  if (capacity < message_length ||
      utp_ntrs_control_encode_header(data, capacity,
                                     UTP_NTRS_CONTROL_NODE_LINK_HELLO,
                                     UTP_NTRS_LINK_HELLO_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  (void)memcpy(data + UTP_NTRS_CONTROL_HEADER_SIZE, &hello->instance,
               sizeof(hello->instance));
  (void)memcpy(data + UTP_NTRS_CONTROL_HEADER_SIZE + sizeof(hello->instance),
               hello->initiator_node_id, sizeof(hello->initiator_node_id));
  (void)memcpy(data + UTP_NTRS_CONTROL_HEADER_SIZE + sizeof(hello->instance) +
                   sizeof(hello->initiator_node_id),
               hello->initiator_nonce, sizeof(hello->initiator_nonce));
  return message_length;
}

bool utp_ntrs_control_decode_link_hello(const uint8_t *data, size_t length,
                                        utp_ntrs_node_link_hello_t *hello) {
  utp_ntrs_control_header_t header;
  const uint8_t *payload;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NODE_LINK_HELLO ||
      header.payload_length != UTP_NTRS_LINK_HELLO_PAYLOAD_SIZE) {
    return false;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  (void)memcpy(&hello->instance, payload, sizeof(hello->instance));
  (void)memcpy(hello->initiator_node_id, payload + sizeof(hello->instance),
               sizeof(hello->initiator_node_id));
  (void)memcpy(hello->initiator_nonce,
               payload + sizeof(hello->instance) +
                   sizeof(hello->initiator_node_id),
               sizeof(hello->initiator_nonce));
  return utp_ntrs_bytes_nonzero(hello->instance.node_id,
                                sizeof(hello->instance.node_id)) &&
         utp_ntrs_bytes_nonzero(hello->instance.boot_id,
                                sizeof(hello->instance.boot_id)) &&
         utp_ntrs_bytes_nonzero(hello->initiator_node_id,
                                sizeof(hello->initiator_node_id)) &&
         utp_ntrs_bytes_nonzero(hello->initiator_nonce,
                                sizeof(hello->initiator_nonce));
}

size_t utp_ntrs_control_encode_forward_filter_response(
    uint8_t *data, size_t capacity,
    const utp_ntrs_forward_filter_response_t *forward) {
  uint8_t *payload;

  if (capacity < UTP_NTRS_CONTROL_HEADER_SIZE +
                     UTP_NTRS_FORWARD_FILTER_RESPONSE_PAYLOAD_SIZE ||
      utp_ntrs_control_encode_header(
          data, capacity, UTP_NTRS_CONTROL_NAT_FORWARD_FILTER_RSP,
          UTP_NTRS_FORWARD_FILTER_RESPONSE_PAYLOAD_SIZE) == 0u) {
    return 0u;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  utp_ntrs_write_u64(payload, forward->forward_id);
  (void)memcpy(payload + 8u, &forward->target, sizeof(forward->target));
  utp_ntrs_encode_endpoint(payload + 40u, &forward->client);
  utp_ntrs_write_u64(payload + 59u, forward->packet_number);
  (void)memcpy(payload + 67u, forward->token, sizeof(forward->token));
  payload[79u] = forward->phase;
  payload[80u] = 0u;
  payload[81u] = 0u;
  payload[82u] = 0u;
  return UTP_NTRS_CONTROL_HEADER_SIZE +
         UTP_NTRS_FORWARD_FILTER_RESPONSE_PAYLOAD_SIZE;
}

bool utp_ntrs_control_decode_forward_filter_response(
    const uint8_t *data, size_t length,
    utp_ntrs_forward_filter_response_t *forward) {
  utp_ntrs_control_header_t header;
  const uint8_t *payload;

  if (!utp_ntrs_control_decode_header(data, length, &header) ||
      header.type != UTP_NTRS_CONTROL_NAT_FORWARD_FILTER_RSP ||
      header.payload_length != UTP_NTRS_FORWARD_FILTER_RESPONSE_PAYLOAD_SIZE) {
    return false;
  }
  payload = data + UTP_NTRS_CONTROL_HEADER_SIZE;
  if (utp_ntrs_read_u64(payload) == 0u ||
      utp_ntrs_read_u64(payload + 59u) == 0u ||
      payload[79u] != UTP_NTRS_NAT_PHASE_CHANGE_IP || payload[80u] != 0u ||
      payload[81u] != 0u || payload[82u] != 0u) {
    return false;
  }
  *forward = (utp_ntrs_forward_filter_response_t){0};
  forward->forward_id = utp_ntrs_read_u64(payload);
  (void)memcpy(&forward->target, payload + 8u, sizeof(forward->target));
  if (!utp_ntrs_node_instance_valid(&forward->target) ||
      !utp_ntrs_decode_endpoint(payload + 40u, &forward->client) ||
      forward->client.port == 0u) {
    return false;
  }
  forward->packet_number = utp_ntrs_read_u64(payload + 59u);
  (void)memcpy(forward->token, payload + 67u, sizeof(forward->token));
  forward->phase = payload[79u];
  return true;
}
