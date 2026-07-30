#ifndef EULAR_UTP_INTERNAL_ADDRESS_H
#define EULAR_UTP_INTERNAL_ADDRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

struct sockaddr;
struct sockaddr_storage;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum utp_address_family {
    UTP_ADDRESS_FAMILY_UNSPECIFIED = 0,
    UTP_ADDRESS_FAMILY_IPV4        = 4,
    UTP_ADDRESS_FAMILY_IPV6        = 6
} utp_address_family_t;

typedef struct utp_address {
    uint8_t  family;
    uint16_t port;
    uint32_t scope_id;
    uint8_t  address[16];
} utp_address_t;

utp_internal_error_t utp_address_parse(utp_address_t *address, const char *text, uint16_t port);
utp_internal_error_t utp_address_from_sockaddr(utp_address_t *address, const struct sockaddr *socket_address,
                                               size_t socket_address_length);
utp_internal_error_t utp_address_to_sockaddr(const utp_address_t *address, struct sockaddr_storage *storage,
                                             size_t *storage_length);
bool                 utp_address_equal(const utp_address_t *left, const utp_address_t *right);
bool                 utp_address_is_unspecified_ipv6(const utp_address_t *address);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_ADDRESS_H
