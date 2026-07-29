#ifndef EULAR_UTP_INTERNAL_UDP_H
#define EULAR_UTP_INTERNAL_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "internal/address.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_UDP_SOCKET_INVALID UINTPTR_MAX

typedef struct utp_udp_socket {
    uintptr_t native_handle;
    bool      winsock_active;
} utp_udp_socket_t;

void                 utp_udp_socket_init(utp_udp_socket_t *udp_socket);
void                 utp_udp_socket_close(utp_udp_socket_t *udp_socket);
bool                 utp_udp_socket_is_open(const utp_udp_socket_t *udp_socket);
utp_internal_error_t utp_udp_socket_open(utp_udp_socket_t *udp_socket, uint8_t address_family);
utp_internal_error_t utp_udp_socket_bind(utp_udp_socket_t *udp_socket, const utp_address_t *requested,
                                         utp_address_t *local);
utp_internal_error_t utp_udp_socket_send_to(utp_udp_socket_t *udp_socket, const void *data, size_t data_length,
                                            const utp_address_t *peer, size_t *sent_length);
utp_internal_error_t utp_udp_socket_recv_from(utp_udp_socket_t *udp_socket, void *data, size_t capacity,
                                              size_t *received_length, utp_address_t *peer);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_UDP_H
