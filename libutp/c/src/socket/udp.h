#ifndef EULAR_UTP_INTERNAL_UDP_H
#define EULAR_UTP_INTERNAL_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_UDP_SOCKET_INVALID         UINTPTR_MAX
#define UTP_UDP_SOCKET_MAX_SEND_SLICES 8u

typedef struct utp_udp_send_slice {
    const void *data;
    size_t      length;
} utp_udp_send_slice_t;

typedef struct utp_udp_socket {
    uintptr_t native_handle;
    bool      winsock_active;
} utp_udp_socket_t;

void                 utp_udp_socket_init(utp_udp_socket_t *udp_socket);
void                 utp_udp_socket_close(utp_udp_socket_t *udp_socket);
bool                 utp_udp_socket_is_open(const utp_udp_socket_t *udp_socket);
utp_internal_error_t utp_udp_socket_open(utp_udp_socket_t *udp_socket, uint8_t address_family);
utp_internal_error_t utp_udp_socket_bind(utp_udp_socket_t *udp_socket, const utp_address_t *requested,
                                         const char *ifname, utp_address_t *local);
utp_internal_error_t utp_udp_socket_send_to(utp_udp_socket_t *udp_socket, const void *data, size_t data_length,
                                            const utp_address_t *peer, size_t *sent_length);
utp_internal_error_t utp_udp_socket_send_to_slices(utp_udp_socket_t *udp_socket, const utp_udp_send_slice_t *slices,
                                                   size_t slice_count, const utp_address_t *peer, size_t *sent_length);
utp_internal_error_t utp_udp_socket_recv_from(utp_udp_socket_t *udp_socket, void *data, size_t capacity,
                                              size_t *received_length, utp_address_t *peer);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_UDP_H
