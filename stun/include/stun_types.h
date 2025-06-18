/*************************************************************************
    > File Name: stun_types.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月11日 星期三 10时55分23秒
 ************************************************************************/

#ifndef __STUN_TYPES_H__
#define __STUN_TYPES_H__

#include <string>

#define STUN_TRX_ID_SIZE    12  /* size of the STUN transaction ID */
#define ENUM_CLASS(em)      (uint32_t)(em)

enum class StunMsgType {
    /* Type                                | Value    | Reference */
    STUN_BINDING_REQUEST                   = 0x0001, /* RFC 5389  */
    STUN_BINDING_RESPONSE                  = 0x0101, /* RFC 5389  */
    STUN_BINDING_ERROR_RESPONSE            = 0x0111, /* RFC 5389  */
    STUN_BINDING_INDICATION                = 0x0011, /* RFC 5389  */
    STUN_SHARED_SECRET_REQUEST             = 0x0002, /* RFC 5389  */
    STUN_SHARED_SECRET_RESPONSE            = 0x0102, /* RFC 5389  */
    STUN_SHARED_SECRET_ERROR_RESPONSE      = 0x0112, /* RFC 5389  */
    STUN_ALLOCATE_REQUEST                  = 0x0003, /* RFC 5766  */
    STUN_ALLOCATE_RESPONSE                 = 0x0103, /* RFC 5766  */
    STUN_ALLOCATE_ERROR_RESPONSE           = 0x0113, /* RFC 5766  */
    STUN_REFRESH_REQUEST                   = 0x0004, /* RFC 5766  */
    STUN_REFRESH_RESPONSE                  = 0x0104, /* RFC 5766  */
    STUN_REFRESH_ERROR_RESPONSE            = 0x0114, /* RFC 5766  */
    STUN_SEND_INDICATION                   = 0x0016, /* RFC 5766  */
    STUN_DATA_INDICATION                   = 0x0017, /* RFC 5766  */
    STUN_CREATE_PERM_REQUEST               = 0x0008, /* RFC 5766  */
    STUN_CREATE_PERM_RESPONSE              = 0x0108, /* RFC 5766  */
    STUN_CREATE_PERM_ERROR_RESPONSE        = 0x0118, /* RFC 5766  */
    STUN_CHANNEL_BIND_REQUEST              = 0x0009, /* RFC 5766  */
    STUN_CHANNEL_BIND_RESPONSE             = 0x0109, /* RFC 5766  */
    STUN_CHANNEL_BIND_ERROR_RESPONSE       = 0x0119, /* RFC 5766  */
    STUN_CONNECT_REQUEST                   = 0x000A, /* RFC 6062  */
    STUN_CONNECT_RESPONSE                  = 0x010A, /* RFC 6062  */
    STUN_CONNECT_ERROR_RESPONSE            = 0x011A, /* RFC 6062  */
    STUN_CONNECTION_BIND_REQUEST           = 0x000B, /* RFC 6062  */
    STUN_CONNECTION_BIND_RESPONSE          = 0x010B, /* RFC 6062  */
    STUN_CONNECTION_BIND_ERROR_RESPONSE    = 0x011B, /* RFC 6062  */
    STUN_CONNECTION_ATTEMPT_REQUEST        = 0x000C, /* RFC 6062  */
    STUN_CONNECTION_ATTEMPT_RESPONSE       = 0x010C, /* RFC 6062  */
    STUN_CONNECTION_ATTEMPT_ERROR_RESPONSE = 0x011C, /* RFC 6062  */
};

enum class StunAttributeType {
    /* Attribute                  | Value   | Type                  | Reference */
    STUN_ATTR_MAPPED_ADDRESS      = 0x0001, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_RESPONSE_ADDRESS    = 0x0002, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_CHANGE_REQUEST      = 0x0003, /* uint32               | RFC 5780  */
    STUN_ATTR_SOURCE_ADDRESS      = 0x0004, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_CHANGED_ADDRESS     = 0x0005, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_USERNAME            = 0x0006, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_PASSWORD            = 0x0007, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_MESSAGE_INTEGRITY   = 0x0008, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_ERROR_CODE          = 0x0009, /* StunAttrErrorCode    | RFC 5389  */
    STUN_ATTR_UNKNOWN_ATTRIBUTES  = 0x000A, /* std::vector<uint16>  | RFC 5389  */
    STUN_ATTR_REFLECTED_FROM      = 0x000B, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_CHANNEL_NUMBER      = 0x000C, /* uint32               | RFC 5766  */
    STUN_ATTR_LIFETIME            = 0x000D, /* uint32               | RFC 5766  */
    STUN_ATTR_BANDWIDTH           = 0x0010, /* uint32               | RFC 5766  */
    STUN_ATTR_XOR_PEER_ADDRESS    = 0x0012, /* SocketAddress        | RFC 5766  */
    STUN_ATTR_DATA                = 0x0013, /* StunAttrVarSize      | RFC 5766  */
    STUN_ATTR_REALM               = 0x0014, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_NONCE               = 0x0015, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_XOR_RELAYED_ADDRESS = 0x0016, /* SocketAddress        | RFC 5766  */
    STUN_ATTR_REQ_ADDRESS_FAMILY  = 0x0017, /* uint8                | RFC 6156  */
    STUN_ATTR_EVEN_PORT           = 0x0018, /* uint8_pad            | RFC 5766  */
    STUN_ATTR_REQUESTED_TRANSPORT = 0x0019, /* uint32               | RFC 5766  */
    STUN_ATTR_DONT_FRAGMENT       = 0x001A, /* empty                | RFC 5766  */
    STUN_ATTR_XOR_MAPPED_ADDRESS  = 0x0020, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_TIMER_VAL           = 0x0021, /* uint32               | RFC 5766  */
    STUN_ATTR_RESERVATION_TOKEN   = 0x0022, /* uint64               | RFC 5766  */
    STUN_ATTR_PRIORITY            = 0x0024, /* uint32               | RFC 5245  */
    STUN_ATTR_USE_CANDIDATE       = 0x0025, /* empty                | RFC 5245  */
    STUN_ATTR_PADDING             = 0x0026, /* StunAttrVarSize      | RFC 5780  */
    STUN_ATTR_RESPONSE_PORT       = 0x0027, /* stun_attr_uint16_pad | RFC 5780  */
    STUN_ATTR_CONNECTION_ID       = 0x002A, /* uint32               | RFC 6062  */
    STUN_ATTR_SOFTWARE            = 0x8022, /* StunAttrVarSize      | RFC 5389  */
    STUN_ATTR_ALTERNATE_SERVER    = 0x8023, /* SocketAddress        | RFC 5389  */
    STUN_ATTR_FINGERPRINT         = 0x8028, /* uint32               | RFC 5389  */
    STUN_ATTR_ICE_CONTROLLED      = 0x8029, /* uint64               | RFC 5245  */
    STUN_ATTR_ICE_CONTROLLING     = 0x802A, /* uint64               | RFC 5245  */
    STUN_ATTR_RESPONSE_ORIGIN     = 0x802B, /* SocketAddress        | RFC 5780  */
    STUN_ATTR_OTHER_ADDRESS       = 0x802C, /* SocketAddress        | RFC 5780  */
};

enum class StunErrorType {
    /* Code                              | Value | Reference */
    STUN_ERROR_TRY_ALTERNATE             = 300, /* RFC 5389  */
    STUN_ERROR_BAD_REQUEST               = 400, /* RFC 5389  */
    STUN_ERROR_UNAUTHORIZED              = 401, /* RFC 5389  */
    STUN_ERROR_FORBIDDEN                 = 403, /* RFC 5766  */
    STUN_ERROR_UNKNOWN_ATTRIBUTE         = 420, /* RFC 5389  */
    STUN_ERROR_ALLOCATION_MISMATCH       = 437, /* RFC 5766  */
    STUN_ERROR_STALE_NONCE               = 438, /* RFC 5389  */
    STUN_ERROR_ADDR_FAMILY_NOT_SUPP      = 440, /* RFC 6156  */
    STUN_ERROR_WRONG_CREDENTIALS         = 441, /* RFC 5766  */
    STUN_ERROR_UNSUPP_TRANSPORT_PROTO    = 442, /* RFC 5766  */
    STUN_ERROR_PEER_ADD_FAMILY_MISMATCH  = 443, /* RFC 6156  */
    STUN_ERROR_CONNECTION_ALREADY_EXISTS = 446, /* RFC 6062  */
    STUN_ERROR_CONNECTION_FAILURE        = 447, /* RFC 6062  */
    STUN_ERROR_ALLOCATION_QUOTA_REACHED  = 486, /* RFC 5766  */
    STUN_ERROR_ROLE_CONFLICT             = 487, /* RFC 5245  */
    STUN_ERROR_SERVER_ERROR              = 500, /* RFC 5389  */
    STUN_ERROR_INSUFFICIENT_CAPACITY     = 508, /* RFC 5766  */
};

enum class StunAddrFamily {
    STUN_IPV4 = 0x01,
    STUN_IPV6 = 0x02
};

struct StunAttrVarSize {
    std::vector<uint8_t> value;
};

struct StunAttrErrorCode {
    uint16_t    error_code;
    std::string error_reason;
};

#endif // __STUN_TYPES_H__
