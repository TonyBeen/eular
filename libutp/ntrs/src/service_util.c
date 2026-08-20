#include "service_util.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>

static bool utp_ntrs_parse_port(const char *text, uint16_t *port) {
  uint32_t value = 0u;

  if (*text == '\0') {
    return false;
  }
  while (*text != '\0') {
    if (*text < '0' || *text > '9' || value > 6553u) {
      return false;
    }
    value = value * 10u + (uint32_t)(*text - '0');
    if (value > UINT16_MAX) {
      return false;
    }
    ++text;
  }
  if (value == 0u) {
    return false;
  }
  *port = (uint16_t)value;
  return true;
}

bool utp_ntrs_endpoint_parse(const char *text, utp_ntrs_endpoint_t *endpoint) {
  char address[INET6_ADDRSTRLEN];
  const char *port_text;
  size_t address_length;
  uint16_t port;

  *endpoint = (utp_ntrs_endpoint_t){0};
  if (text[0] == '[') {
    const char *const closing = strchr(text + 1, ']');

    if (closing == NULL || closing[1] != ':') {
      return false;
    }
    address_length = (size_t)(closing - (text + 1));
    port_text = closing + 2;
    if (address_length == 0u || address_length >= sizeof(address) ||
        !utp_ntrs_parse_port(port_text, &port)) {
      return false;
    }
    (void)memcpy(address, text + 1, address_length);
    address[address_length] = '\0';
    if (inet_pton(AF_INET6, address, endpoint->address) != 1) {
      return false;
    }
    endpoint->family = (uint8_t)AF_INET6;
  } else {
    const char *const separator = strrchr(text, ':');

    if (separator == NULL) {
      return false;
    }
    address_length = (size_t)(separator - text);
    port_text = separator + 1;
    if (address_length == 0u || address_length >= sizeof(address) ||
        !utp_ntrs_parse_port(port_text, &port)) {
      return false;
    }
    (void)memcpy(address, text, address_length);
    address[address_length] = '\0';
    if (inet_pton(AF_INET, address, endpoint->address) != 1) {
      return false;
    }
    endpoint->family = (uint8_t)AF_INET;
  }
  endpoint->port = port;
  return true;
}

bool utp_ntrs_endpoint_to_sockaddr(const utp_ntrs_endpoint_t *endpoint,
                                   struct sockaddr_storage *storage,
                                   socklen_t *length) {
  *storage = (struct sockaddr_storage){0};
  if (endpoint->family == (uint8_t)AF_INET) {
    struct sockaddr_in *const address = (struct sockaddr_in *)storage;

    address->sin_family = AF_INET;
    address->sin_port = htons(endpoint->port);
    (void)memcpy(&address->sin_addr, endpoint->address,
                 sizeof(address->sin_addr));
    *length = (socklen_t)sizeof(*address);
    return true;
  }
  if (endpoint->family == (uint8_t)AF_INET6) {
    struct sockaddr_in6 *const address = (struct sockaddr_in6 *)storage;

    address->sin6_family = AF_INET6;
    address->sin6_port = htons(endpoint->port);
    (void)memcpy(&address->sin6_addr, endpoint->address,
                 sizeof(address->sin6_addr));
    *length = (socklen_t)sizeof(*address);
    return true;
  }
  return false;
}

const char *utp_ntrs_endpoint_format(const utp_ntrs_endpoint_t *endpoint,
                                     char *buffer, size_t capacity) {
  char address[INET6_ADDRSTRLEN];

  if (endpoint->family == (uint8_t)AF_INET) {
    if (inet_ntop(AF_INET, endpoint->address, address, sizeof(address)) ==
            NULL ||
        (size_t)snprintf(buffer, capacity, "%s:%u", address,
                         (uint32_t)endpoint->port) >= capacity) {
      return NULL;
    }
    return buffer;
  }
  if (endpoint->family == (uint8_t)AF_INET6) {
    if (inet_ntop(AF_INET6, endpoint->address, address, sizeof(address)) ==
            NULL ||
        (size_t)snprintf(buffer, capacity, "[%s]:%u", address,
                         (uint32_t)endpoint->port) >= capacity) {
      return NULL;
    }
    return buffer;
  }
  return NULL;
}

static int32_t utp_ntrs_hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool utp_ntrs_hex_decode(const char *text, uint8_t *output,
                         size_t output_length) {
  size_t index;

  for (index = 0u; index < output_length; ++index) {
    const int32_t high = utp_ntrs_hex_value(text[index * 2u]);
    const int32_t low = utp_ntrs_hex_value(text[index * 2u + 1u]);

    if (high < 0 || low < 0) {
      return false;
    }
    output[index] = (uint8_t)((uint32_t)high << 4u) | (uint8_t)low;
  }
  return text[output_length * 2u] == '\0';
}

const char *
utp_ntrs_node_instance_format(const utp_ntrs_node_instance_t *instance,
                              char *buffer, size_t capacity) {
  static const char digits[] = "0123456789abcdef";
  size_t index;

  if (capacity < UTP_NTRS_NODE_INSTANCE_TEXT_SIZE) {
    return NULL;
  }
  for (index = 0u; index < UTP_NTRS_NODE_ID_SIZE; ++index) {
    buffer[index * 2u] = digits[instance->node_id[index] >> 4u];
    buffer[index * 2u + 1u] = digits[instance->node_id[index] & 0x0fu];
  }
  buffer[UTP_NTRS_NODE_ID_SIZE * 2u] = ':';
  for (index = 0u; index < UTP_NTRS_BOOT_ID_SIZE; ++index) {
    const size_t offset = UTP_NTRS_NODE_ID_SIZE * 2u + 1u + index * 2u;

    buffer[offset] = digits[instance->boot_id[index] >> 4u];
    buffer[offset + 1u] = digits[instance->boot_id[index] & 0x0fu];
  }
  buffer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE - 1u] = '\0';
  return buffer;
}

uint64_t utp_ntrs_now_ms(void) {
  struct timespec now;

  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
