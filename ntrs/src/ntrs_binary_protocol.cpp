#include "ntrs_binary_protocol.h"
#include "ntrs_probe_types.h"

#include <arpa/inet.h>
#include <string.h>

namespace {

static const size_t kNtrsBinaryHeaderSize = sizeof(ntrs_binary_frame_header_t);
static const size_t kNtrsBinaryTlvHeaderSize = sizeof(ntrs_binary_tlv_header_t);
static const size_t kNtrsBinaryIpv4EndpointSize = 8;
static const size_t kNtrsBinaryIpv6EndpointSize = 20;

static void put_u16_be(uint8_t* dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[1] = static_cast<uint8_t>(value & 0xFFu);
}

static void put_u32_be(uint8_t* dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[3] = static_cast<uint8_t>(value & 0xFFu);
}

static void put_u64_be(uint8_t* dst, uint64_t value)
{
    dst[0] = static_cast<uint8_t>((value >> 56) & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 48) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 40) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 32) & 0xFFu);
    dst[4] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    dst[5] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[6] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[7] = static_cast<uint8_t>(value & 0xFFu);
}

static uint16_t get_u16_be(const uint8_t* src)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]));
}

static uint32_t get_u32_be(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

static uint64_t get_u64_be(const uint8_t* src)
{
    return (static_cast<uint64_t>(src[0]) << 56) | (static_cast<uint64_t>(src[1]) << 48) |
           (static_cast<uint64_t>(src[2]) << 40) | (static_cast<uint64_t>(src[3]) << 32) |
           (static_cast<uint64_t>(src[4]) << 24) | (static_cast<uint64_t>(src[5]) << 16) |
           (static_cast<uint64_t>(src[6]) << 8) | static_cast<uint64_t>(src[7]);
}

} // namespace

bool ntrs_binary_frame_init(ntrs_binary_frame_t* frame, uint8_t* buffer, size_t capacity)
{
    if (frame == NULL || buffer == NULL || capacity < kNtrsBinaryHeaderSize) {
        return false;
    }

    frame->buffer = buffer;
    frame->capacity = capacity;
    frame->length = 0;
    memset(frame->buffer, 0, frame->capacity);
    return true;
}

bool ntrs_binary_frame_set_header(ntrs_binary_frame_t* frame, ntrs_binary_frame_type_t frame_type,
                                  ntrs_binary_phase_t phase, uint8_t flags, uint32_t request_id, uint32_t sequence,
                                  uint64_t timestamp_ms)
{
    uint8_t* cursor = NULL;

    if (frame == NULL || frame->buffer == NULL || frame->capacity < kNtrsBinaryHeaderSize ||
        !ntrs_binary_frame_type_is_valid(frame_type) || !ntrs_binary_phase_is_valid(phase)) {
        return false;
    }

    cursor = frame->buffer;
    put_u32_be(cursor, NTRS_BINARY_FRAME_MAGIC);
    cursor += sizeof(uint32_t);
    *cursor++ = NTRS_BINARY_FRAME_VERSION;
    *cursor++ = static_cast<uint8_t>(frame_type);
    *cursor++ = static_cast<uint8_t>(phase);
    *cursor++ = flags;
    put_u32_be(cursor, request_id);
    cursor += sizeof(uint32_t);
    put_u32_be(cursor, sequence);
    cursor += sizeof(uint32_t);
    put_u64_be(cursor, timestamp_ms);

    frame->length = kNtrsBinaryHeaderSize;
    return true;
}

bool ntrs_binary_frame_add_tlv(ntrs_binary_frame_t* frame, ntrs_binary_tlv_type_t type, const void* value,
                               uint16_t value_len)
{
    uint8_t* cursor = NULL;

    if (frame == NULL || frame->buffer == NULL || frame->length < kNtrsBinaryHeaderSize || value == NULL ||
        type == NTRS_BINARY_TLV_UNKNOWN) {
        return false;
    }
    if (frame->length + kNtrsBinaryTlvHeaderSize + value_len > frame->capacity) {
        return false;
    }

    cursor = frame->buffer + frame->length;
    put_u16_be(cursor, static_cast<uint16_t>(type));
    cursor += sizeof(uint16_t);
    put_u16_be(cursor, value_len);
    cursor += sizeof(uint16_t);
    memcpy(cursor, value, value_len);
    frame->length += kNtrsBinaryTlvHeaderSize + value_len;
    return true;
}

bool ntrs_binary_frame_parse(const uint8_t* data, size_t data_len, ntrs_binary_frame_view_t* view)
{
    if (data == NULL || view == NULL || data_len < kNtrsBinaryHeaderSize) {
        return false;
    }

    memset(view, 0, sizeof(*view));
    view->header.magic = get_u32_be(data);
    if (view->header.magic != NTRS_BINARY_FRAME_MAGIC) {
        return false;
    }

    view->header.version = data[4];
    view->header.frame_type = data[5];
    view->header.phase = data[6];
    view->header.flags = data[7];
    view->header.request_id = get_u32_be(data + 8);
    view->header.sequence = get_u32_be(data + 12);
    view->header.timestamp_ms = get_u64_be(data + 16);
    if (view->header.version != NTRS_BINARY_FRAME_VERSION ||
        !ntrs_binary_frame_type_is_valid(static_cast<ntrs_binary_frame_type_t>(view->header.frame_type)) ||
        !ntrs_binary_phase_is_valid(static_cast<ntrs_binary_phase_t>(view->header.phase))) {
        return false;
    }

    view->payload = data + kNtrsBinaryHeaderSize;
    view->payload_len = data_len - kNtrsBinaryHeaderSize;
    return true;
}

bool ntrs_binary_frame_next_tlv(const ntrs_binary_frame_view_t* view, size_t* cursor, ntrs_binary_tlv_view_t* tlv)
{
    size_t offset = 0;
    uint16_t type = 0;
    uint16_t value_len = 0;

    if (view == NULL || cursor == NULL || tlv == NULL) {
        return false;
    }

    offset = *cursor;
    if (offset >= view->payload_len) {
        return false;
    }
    if (view->payload_len - offset < kNtrsBinaryTlvHeaderSize) {
        return false;
    }

    type = get_u16_be(view->payload + offset);
    value_len = get_u16_be(view->payload + offset + sizeof(uint16_t));
    offset += kNtrsBinaryTlvHeaderSize;
    if (view->payload_len - offset < value_len) {
        return false;
    }

    tlv->type = static_cast<ntrs_binary_tlv_type_t>(type);
    tlv->value = view->payload + offset;
    tlv->value_len = value_len;
    *cursor = offset + value_len;
    return true;
}

bool ntrs_binary_frame_find_tlv(const ntrs_binary_frame_view_t* view, ntrs_binary_tlv_type_t type,
                                ntrs_binary_tlv_view_t* tlv)
{
    size_t cursor = 0;

    if (view == NULL || tlv == NULL || type == NTRS_BINARY_TLV_UNKNOWN) {
        return false;
    }

    while (ntrs_binary_frame_next_tlv(view, &cursor, tlv)) {
        if (tlv->type == type) {
            return true;
        }
    }
    return false;
}

bool ntrs_binary_frame_add_endpoint_tlv(ntrs_binary_frame_t* frame, ntrs_binary_tlv_type_t type,
                                        const struct sockaddr* addr, socklen_t addr_len)
{
    uint8_t                buffer[kNtrsBinaryIpv6EndpointSize];
    uint16_t               payload_len = 0;
    const struct sockaddr_in*  addr4 = NULL;
    const struct sockaddr_in6* addr6 = NULL;

    (void)addr_len;

    if (frame == NULL || addr == NULL) {
        return false;
    }

    memset(buffer, 0, sizeof(buffer));
    if (addr->sa_family == AF_INET) {
        addr4 = reinterpret_cast<const struct sockaddr_in*>(addr);
        buffer[0] = static_cast<uint8_t>(NTRS_BINARY_ADDR_FAMILY_IPV4);
        put_u16_be(buffer + 2, ntohs(addr4->sin_port));
        memcpy(buffer + 4, &addr4->sin_addr, sizeof(addr4->sin_addr));
        payload_len = static_cast<uint16_t>(kNtrsBinaryIpv4EndpointSize);
    } else if (addr->sa_family == AF_INET6) {
        addr6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
        buffer[0] = static_cast<uint8_t>(NTRS_BINARY_ADDR_FAMILY_IPV6);
        put_u16_be(buffer + 2, ntohs(addr6->sin6_port));
        memcpy(buffer + 4, &addr6->sin6_addr, sizeof(addr6->sin6_addr));
        payload_len = static_cast<uint16_t>(kNtrsBinaryIpv6EndpointSize);
    } else {
        return false;
    }

    return ntrs_binary_frame_add_tlv(frame, type, buffer, payload_len);
}

bool ntrs_binary_tlv_parse_endpoint(const ntrs_binary_tlv_view_t* tlv, struct sockaddr_storage* addr,
                                    socklen_t* addr_len)
{
    struct sockaddr_in*  addr4 = NULL;
    struct sockaddr_in6* addr6 = NULL;
    uint8_t              family = 0;
    uint16_t             port = 0;

    if (tlv == NULL || addr == NULL || addr_len == NULL || tlv->value == NULL || tlv->value_len < 4) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));
    family = tlv->value[0];
    port = get_u16_be(tlv->value + 2);
    if (family == NTRS_BINARY_ADDR_FAMILY_IPV4 && tlv->value_len == kNtrsBinaryIpv4EndpointSize) {
        addr4 = reinterpret_cast<struct sockaddr_in*>(addr);
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        memcpy(&addr4->sin_addr, tlv->value + 4, sizeof(addr4->sin_addr));
        *addr_len = static_cast<socklen_t>(sizeof(*addr4));
        return true;
    }
    if (family == NTRS_BINARY_ADDR_FAMILY_IPV6 && tlv->value_len == kNtrsBinaryIpv6EndpointSize) {
        addr6 = reinterpret_cast<struct sockaddr_in6*>(addr);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        memcpy(&addr6->sin6_addr, tlv->value + 4, sizeof(addr6->sin6_addr));
        *addr_len = static_cast<socklen_t>(sizeof(*addr6));
        return true;
    }
    return false;
}

bool ntrs_binary_frame_type_is_valid(ntrs_binary_frame_type_t frame_type)
{
    switch (frame_type) {
    case NTRS_BINARY_FRAME_PROBE_REQ:
    case NTRS_BINARY_FRAME_PROBE_RSP:
    case NTRS_BINARY_FRAME_FILTER_REQ:
    case NTRS_BINARY_FRAME_FILTER_RSP:
    case NTRS_BINARY_FRAME_PUNCH_REQ:
    case NTRS_BINARY_FRAME_PUNCH_ACK:
        return true;
    case NTRS_BINARY_FRAME_UNKNOWN:
    default:
        return false;
    }
}

bool ntrs_binary_phase_is_valid(ntrs_binary_phase_t phase)
{
    switch (phase) {
    case NTRS_BINARY_PHASE_PROBE1:
    case NTRS_BINARY_PHASE_CHANGE_PORT:
    case NTRS_BINARY_PHASE_CHANGE_IP:
    case NTRS_BINARY_PHASE_PROBE2:
    case NTRS_BINARY_PHASE_PUNCH:
        return true;
    case NTRS_BINARY_PHASE_UNKNOWN:
    default:
        return false;
    }
}

void ntrs_probe_token_init(ntrs_probe_token_t* token)
{
    if (token == NULL) {
        return;
    }
    memset(token, 0, sizeof(*token));
}

void ntrs_probe_result_init(ntrs_probe_result_t* result)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
}
