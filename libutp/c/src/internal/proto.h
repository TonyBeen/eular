#ifndef EULAR_UTP_INTERNAL_PROTO_H
#define EULAR_UTP_INTERNAL_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_PACKET_HEADER_SIZE 20u
#define UTP_PROTOCOL_VERSION   2u
#define UTP_PACKET_MTU_FLOOR   1280u
#define UTP_PACKET_NUMBER_MAX  UINT64_C(0x3fffffffffffffff)

typedef enum utp_packet_type {
    UTP_PACKET_TYPE_NONE             = 0x00,
    UTP_PACKET_TYPE_INITIAL          = 0x01,
    UTP_PACKET_TYPE_HANDSHAKE        = 0x02,
    UTP_PACKET_TYPE_0RTT             = 0x03,
    UTP_PACKET_TYPE_CONNECTION_CLOSE = 0x04,
    UTP_PACKET_TYPE_CTRL             = 0x05,
    UTP_PACKET_TYPE_CONNECT          = 0x06
} utp_packet_type_t;

typedef struct utp_packet_header {
    uint32_t scid;
    uint32_t dcid;
    uint64_t packet_number;
    uint16_t payload_length;
    uint8_t  type;
    uint8_t  reserve;
} utp_packet_header_t;

utp_internal_error_t utp_proto_encode_header(uint8_t *buffer, size_t capacity, const utp_packet_header_t *header);
utp_internal_error_t utp_proto_decode_header(utp_packet_header_t *header, const uint8_t *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PROTO_H
