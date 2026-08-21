#include "service_util.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
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

bool utp_ntrs_endpoint_resolve_for_family(const char *text, int32_t family,
                                          utp_ntrs_endpoint_t *endpoint) {
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
  struct addrinfo *addresses;
  struct addrinfo *result;
  const char *separator;
  char host[256];
  char service[6];
  size_t host_length;
  uint16_t port;
  utp_ntrs_endpoint_t parsed;

  if ((family != AF_INET && family != AF_INET6) || text == NULL || endpoint == NULL) {
    return false;
  }
  if (utp_ntrs_endpoint_parse(text, &parsed)) {
    if (parsed.family != (uint8_t)family) {
      return false;
    }
    *endpoint = parsed;
    return true;
  }
  separator = strrchr(text, ':');
  if (separator == NULL || separator == text || separator[1] == '\0' ||
      !utp_ntrs_parse_port(separator + 1, &port)) {
    return false;
  }
  host_length = (size_t)(separator - text);
  if (text[0] == '[' && host_length > 2u && text[host_length - 1u] == ']') {
    ++text;
    host_length -= 2u;
  }
  if (host_length == 0u || host_length >= sizeof(host) ||
      (size_t)snprintf(service, sizeof(service), "%u", (uint32_t)port) >=
          sizeof(service)) {
    return false;
  }
  (void)memcpy(host, text, host_length);
  host[host_length] = '\0';
  hints.ai_family = family;
  if (getaddrinfo(host, service, &hints, &addresses) != 0) {
    return false;
  }
  for (result = addresses; result != NULL; result = result->ai_next) {
    if (family == AF_INET && result->ai_family == AF_INET) {
      const struct sockaddr_in *const address =
          (const struct sockaddr_in *)result->ai_addr;

      *endpoint = (utp_ntrs_endpoint_t){
          .family = (uint8_t)AF_INET,
          .port = ntohs(address->sin_port),
      };
      (void)memcpy(endpoint->address, &address->sin_addr,
                   sizeof(address->sin_addr));
      freeaddrinfo(addresses);
      return true;
    }
    if (family == AF_INET6 && result->ai_family == AF_INET6) {
      const struct sockaddr_in6 *const address =
          (const struct sockaddr_in6 *)result->ai_addr;

      if (IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)) {
        continue;
      }
      *endpoint = (utp_ntrs_endpoint_t){
          .family = (uint8_t)AF_INET6,
          .port = ntohs(address->sin6_port),
      };
      (void)memcpy(endpoint->address, &address->sin6_addr,
                   sizeof(address->sin6_addr));
      freeaddrinfo(addresses);
      return true;
    }
  }
  freeaddrinfo(addresses);
  return false;
}

bool utp_ntrs_endpoint_resolve(const char *text, utp_ntrs_endpoint_t *endpoint) {
  return utp_ntrs_endpoint_resolve_for_family(text, AF_INET, endpoint) ||
         utp_ntrs_endpoint_resolve_for_family(text, AF_INET6, endpoint);
}

bool utp_ntrs_endpoint_from_sockaddr(utp_ntrs_endpoint_t *endpoint,
                                     const struct sockaddr *address,
                                     socklen_t length) {
  *endpoint = (utp_ntrs_endpoint_t){0};
  if (address->sa_family == AF_INET && length >= (socklen_t)sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *const ipv4 = (const struct sockaddr_in *)address;

    endpoint->family = (uint8_t)AF_INET;
    endpoint->port = ntohs(ipv4->sin_port);
    (void)memcpy(endpoint->address, &ipv4->sin_addr, sizeof(ipv4->sin_addr));
    return true;
  }
  if (address->sa_family == AF_INET6 && length >= (socklen_t)sizeof(struct sockaddr_in6)) {
    const struct sockaddr_in6 *const ipv6 = (const struct sockaddr_in6 *)address;

    endpoint->family = (uint8_t)AF_INET6;
    endpoint->port = ntohs(ipv6->sin6_port);
    (void)memcpy(endpoint->address, &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
    return true;
  }
  return false;
}

bool utp_ntrs_socket_bind_interface(int32_t fd, const char *interface_name) {
  const size_t length = interface_name != NULL ? strlen(interface_name) : 0u;

  return interface_name == NULL ||
         (length != 0u && length < (size_t)IFNAMSIZ &&
          setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                     (socklen_t)(length + 1u)) == 0);
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
