#ifndef EULAR_UTP_INTERNAL_UDP_H
#define EULAR_UTP_INTERNAL_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "socket/address.h"
#include "util/error.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <mswsock.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_UDP_SOCKET_INVALID         UINTPTR_MAX
#define UTP_UDP_SOCKET_MAX_SEND_SLICES 8u

typedef struct utp_udp_send_slice {
    const void* data;    // 借用的发送数据
    size_t      length;  // 数据长度
} utp_udp_send_slice_t;

typedef struct utp_udp_socket {
    uintptr_t native_handle;  // 平台 UDP socket 句柄
#if defined(_WIN32)
    LPFN_WSARECVMSG wsa_recv_msg;
    LPFN_WSASENDMSG wsa_send_msg;
#endif
    uint16_t local_port;  // bind 后的本地端口，供目的地址元数据补全
} utp_udp_socket_t;

void                 utp_udp_socket_init(utp_udp_socket_t* udp_socket);
void                 utp_udp_socket_close(utp_udp_socket_t* udp_socket);
bool                 utp_udp_socket_is_open(const utp_udp_socket_t* udp_socket);
utp_internal_error_t utp_udp_socket_open(utp_udp_socket_t* udp_socket, uint8_t address_family);
utp_internal_error_t utp_udp_socket_bind(utp_udp_socket_t* udp_socket, const utp_address_t* requested,
                                         const char* ifname, utp_address_t* local);
utp_internal_error_t utp_udp_socket_send_to(utp_udp_socket_t* udp_socket, const void* data, size_t data_length,
                                            const utp_address_t* peer, size_t* sent_length);
utp_internal_error_t utp_udp_socket_send_from_to(utp_udp_socket_t* udp_socket, const void* data, size_t data_length,
                                                 const utp_address_t* peer, const utp_address_t* local,
                                                 size_t* sent_length);
utp_internal_error_t utp_udp_socket_send_to_slices(utp_udp_socket_t* udp_socket, const utp_udp_send_slice_t* slices,
                                                   size_t slice_count, const utp_address_t* peer, size_t* sent_length);
utp_internal_error_t utp_udp_socket_send_from_to_slices(utp_udp_socket_t*           udp_socket,
                                                        const utp_udp_send_slice_t* slices, size_t slice_count,
                                                        const utp_address_t* peer, const utp_address_t* local,
                                                        size_t* sent_length);
utp_internal_error_t utp_udp_socket_recv_from(utp_udp_socket_t* udp_socket, void* data, size_t capacity,
                                              size_t* received_length, utp_address_t* peer);
utp_internal_error_t utp_udp_socket_recv_from_ex(utp_udp_socket_t* udp_socket, void* data, size_t capacity,
                                                 size_t* received_length, utp_address_t* peer, utp_address_t* local);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_UDP_H
