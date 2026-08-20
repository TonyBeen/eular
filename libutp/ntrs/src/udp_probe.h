#ifndef EULAR_NTRS_UDP_PROBE_H
#define EULAR_NTRS_UDP_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ntrs/service.h>

typedef enum utp_ntrs_udp_reply_socket {
  UTP_NTRS_UDP_REPLY_PROBE = 0,
  UTP_NTRS_UDP_REPLY_CHANGE_PORT = 1,
} utp_ntrs_udp_reply_socket_t;

/** @brief 解析一个 NAT 请求并构造对应回包；返回 false 表示静默丢弃。 */
bool utp_ntrs_udp_handle_probe_request(
    const uint8_t *request, size_t request_length,
    const utp_ntrs_endpoint_t *client_endpoint,
    const utp_ntrs_endpoint_t *probe_endpoint,
    const utp_ntrs_endpoint_t *change_port_endpoint,
    const utp_ntrs_endpoint_t *alternate_probe_endpoint, uint8_t *response,
    size_t response_capacity, size_t *response_length,
    utp_ntrs_udp_reply_socket_t *reply_socket);

#endif // EULAR_NTRS_UDP_PROBE_H
