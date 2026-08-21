#include "udp_probe.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "proto/proto.h"
#include "service_util.h"

#define UTP_NTRS_NAT_PACKET_SIZE            128u
#define UTP_NTRS_UTP_PACKET_TYPE_NAT_PROBE  0x07u
#define UTP_NTRS_NAT_PROBE_VERSION          1u
#define UTP_NTRS_NAT_PROBE_TOKEN_SIZE       12u
#define UTP_NTRS_NAT_MESSAGE_PROBE_REQ      1u
#define UTP_NTRS_NAT_MESSAGE_PROBE_RSP      2u
#define UTP_NTRS_NAT_MESSAGE_FILTER_REQ     3u
#define UTP_NTRS_NAT_MESSAGE_FILTER_RSP     4u
#define UTP_NTRS_NAT_PHASE_PROBE1           1u
#define UTP_NTRS_NAT_PHASE_CHANGE_PORT      2u
#define UTP_NTRS_NAT_PHASE_CHANGE_IP        3u
#define UTP_NTRS_NAT_PHASE_PROBE2           4u
#define UTP_NTRS_NAT_TLV_PROBE_TOKEN        1u
#define UTP_NTRS_NAT_TLV_MAPPED_ADDR        6u
#define UTP_NTRS_NAT_TLV_ORIGIN_ADDR        7u
#define UTP_NTRS_NAT_TLV_ALTERNATE_ENDPOINT 8u
#define UTP_NTRS_NAT_TLV_PADDING            12u
#define UTP_NTRS_UDP_READ_BATCH             64u
#define UTP_NTRS_AFFINITY_CAPACITY          1024u
#define UTP_NTRS_AFFINITY_LIFETIME_MS       30000u
#define UTP_NTRS_SOURCE_RATE_SHARD_COUNT    64u
#define UTP_NTRS_SOURCE_RATE_SHARD_CAPACITY 64u
#define UTP_NTRS_SOURCE_RATE_IDLE_MS        60000u
#define UTP_NTRS_SOURCE_RATE_TOKEN_SCALE    1000u
#define UTP_NTRS_SOURCE_RATE_MAX_PER_SECOND 1000000u
#define UTP_NTRS_SOURCE_RATE_MAX_BURST      1000000u

typedef struct utp_ntrs_udp_worker utp_ntrs_udp_worker_t;

typedef struct utp_ntrs_probe_request {
    const uint8_t* token;          // 零拷贝请求 token
    uint64_t       packet_number;  // 请求 UTP 包号
    uint8_t        phase;          // NAT 探测阶段
} utp_ntrs_probe_request_t;

typedef struct utp_ntrs_udp_affinity {
    utp_ntrs_endpoint_t      client;         // 客户端源 endpoint
    utp_ntrs_endpoint_t      primary_probe;  // 已下发的协同 probe endpoint
    utp_ntrs_node_instance_t primary;        // 已下发的协同 Node 实例
    uint64_t                 expires_at_ms;  // 亲和项失效时刻
} utp_ntrs_udp_affinity_t;

typedef struct utp_ntrs_source_rate_entry {
    uint8_t  family;          // AF_INET 或 AF_INET6
    uint8_t  address[16];     // IPv4 仅使用前 4 字节
    uint64_t tokens;          // 以毫令牌计的当前额度
    uint64_t last_refill_ms;  // 上次补充令牌的时刻
    uint64_t last_seen_ms;    // 最近一次请求时刻，用于回收
} utp_ntrs_source_rate_entry_t;

typedef struct utp_ntrs_source_rate_shard {
    utp_ntrs_source_rate_entry_t entries[UTP_NTRS_SOURCE_RATE_SHARD_CAPACITY];  // 固定容量源 IP 表
    pthread_mutex_t              lock;                                          // 分片并发保护
} utp_ntrs_source_rate_shard_t;

typedef struct utp_ntrs_udp_forward_item {
    struct utp_ntrs_udp_forward_item*  next;     // 跨线程队列链表
    utp_ntrs_forward_filter_response_t forward;  // 投递给主 loop 的请求
} utp_ntrs_udp_forward_item_t;

typedef struct utp_ntrs_udp_socket_context {
    utp_ntrs_udp_worker_t*      worker;       // 所属 worker
    utp_ntrs_udp_reply_socket_t socket_kind;  // 本事件对应的本地 socket
} utp_ntrs_udp_socket_context_t;

struct utp_ntrs_udp_worker {
    struct utp_ntrs_udp_server*   server;               // 所属 UDP 服务
    struct event_base*            base;                 // 当前 worker 独享的 libevent loop
    struct event*                 probe_event;          // probe socket 的可读事件
    struct event*                 change_port_event;    // change-port socket 的可读事件
    utp_ntrs_udp_socket_context_t probe_context;        // probe 事件参数
    utp_ntrs_udp_socket_context_t change_port_context;  // change-port 事件参数
    utp_ntrs_udp_server_options_t options;              // 只读 worker 配置副本
    utp_ntrs_udp_affinity_t*      affinities;           // 本 worker 的客户端亲和表
    int32_t                       probe_fd;             // 绑定 probe endpoint 的 SO_REUSEPORT socket
    int32_t                       change_port_fd;       // 绑定 change-port endpoint 的 SO_REUSEPORT socket
    pthread_t                     thread;               // 独立事件循环线程
    bool                          started : 1;          // pthread 是否已启动
};

struct utp_ntrs_udp_server {
    utp_ntrs_udp_worker_t*       workers;            // worker 动态数组
    struct event*                forward_event;      // 主 loop 的本地 socket 可读事件
    utp_ntrs_udp_forward_item_t* forward_head;       // worker -> 主 loop 队列头
    utp_ntrs_udp_forward_item_t* forward_tail;       // worker -> 主 loop 队列尾
    void*                        forward_user_data;  // 主 loop 回调上下文
    utp_ntrs_udp_forward_fn      on_forward;         // 主 loop 转发回调
    pthread_mutex_t              forward_lock;       // 转发队列并发保护
    uint16_t                     worker_count;       // worker 数量
    int32_t                      forward_read_fd;    // 主 loop 监听的本地 socket
    int32_t                      forward_write_fd;   // worker 写入的本地 socket
    pthread_rwlock_t             alternate_lock;     // primary 探测 endpoint 的并发保护
    utp_ntrs_source_rate_shard_t
             source_rate_shards[UTP_NTRS_SOURCE_RATE_SHARD_COUNT];  // 全部 worker 共享的源 IP 限速表
    uint32_t source_rate_per_second;                                // 每源 IP 稳态额度
    uint32_t source_burst;                                          // 每源 IP 突发额度
};

static pthread_once_t utp_ntrs_event_thread_once = PTHREAD_ONCE_INIT;
static int32_t        utp_ntrs_event_thread_status;

static void           utp_ntrs_enable_event_threads(void) { utp_ntrs_event_thread_status = evthread_use_pthreads(); }

static uint16_t       utp_ntrs_read_u16(const uint8_t* data)
{
    return (uint16_t)(((uint16_t)data[0] << 8u) | (uint16_t)data[1]);
}

static void utp_ntrs_write_u16(uint8_t* data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

static bool utp_ntrs_endpoint_same_ip(const utp_ntrs_endpoint_t* left, const utp_ntrs_endpoint_t* right)
{
    const size_t address_length = left->family == (uint8_t)AF_INET ? 4u : 16u;

    return left->family == right->family && memcmp(left->address, right->address, address_length) == 0;
}

static uint32_t utp_ntrs_source_rate_hash(const utp_ntrs_endpoint_t* endpoint)
{
    const size_t address_length = endpoint->family == (uint8_t)AF_INET ? 4u : 16u;
    uint32_t     value          = UINT32_C(2166136261);
    size_t       index;

    value ^= endpoint->family;
    value *= UINT32_C(16777619);
    for (index = 0u; index < address_length; ++index) {
        value ^= endpoint->address[index];
        value *= UINT32_C(16777619);
    }
    return value;
}

static bool utp_ntrs_source_rate_entry_matches(const utp_ntrs_source_rate_entry_t* entry,
                                               const utp_ntrs_endpoint_t*          endpoint)
{
    const size_t address_length = endpoint->family == (uint8_t)AF_INET ? 4u : 16u;

    return entry->family == endpoint->family && memcmp(entry->address, endpoint->address, address_length) == 0;
}

/** @brief 消费一个源 IP 请求额度；所有 UDP worker 共享同一分片表。 */
static bool utp_ntrs_source_rate_allows(utp_ntrs_udp_server_t* server, const utp_ntrs_endpoint_t* endpoint,
                                        uint64_t now_ms)
{
    const uint32_t                      hash   = utp_ntrs_source_rate_hash(endpoint);
    utp_ntrs_source_rate_shard_t* const shard  = &server->source_rate_shards[hash % UTP_NTRS_SOURCE_RATE_SHARD_COUNT];
    const uint64_t                burst_tokens = (uint64_t)server->source_burst * UTP_NTRS_SOURCE_RATE_TOKEN_SCALE;
    utp_ntrs_source_rate_entry_t* entry        = NULL;
    utp_ntrs_source_rate_entry_t* replacement  = NULL;
    uint32_t                      index;
    bool                          allowed;

    (void)pthread_mutex_lock(&shard->lock);
    for (index = 0u; index < UTP_NTRS_SOURCE_RATE_SHARD_CAPACITY; ++index) {
        utp_ntrs_source_rate_entry_t* const current = &shard->entries[index];

        if (current->family != 0u && utp_ntrs_source_rate_entry_matches(current, endpoint)) {
            entry = current;
            break;
        }
        if (replacement == NULL || current->family == 0u ||
            (current->last_seen_ms <= now_ms && now_ms - current->last_seen_ms >= UTP_NTRS_SOURCE_RATE_IDLE_MS) ||
            current->last_seen_ms < replacement->last_seen_ms) {
            replacement = current;
        }
    }
    if (entry == NULL) {
        entry  = replacement;
        *entry = (utp_ntrs_source_rate_entry_t){
            .family         = endpoint->family,
            .tokens         = burst_tokens,
            .last_refill_ms = now_ms,
            .last_seen_ms   = now_ms,
        };
        (void)memcpy(entry->address, endpoint->address, endpoint->family == (uint8_t)AF_INET ? 4u : 16u);
    } else if (now_ms > entry->last_refill_ms) {
        const uint64_t elapsed_ms = now_ms - entry->last_refill_ms;
        const uint64_t refill     = elapsed_ms > UINT64_MAX / server->source_rate_per_second
                                        ? UINT64_MAX
                                        : elapsed_ms * server->source_rate_per_second;

        entry->tokens         = refill >= burst_tokens - entry->tokens ? burst_tokens : entry->tokens + refill;
        entry->last_refill_ms = now_ms;
    }
    entry->last_seen_ms = now_ms;
    allowed             = entry->tokens >= UTP_NTRS_SOURCE_RATE_TOKEN_SCALE;
    if (allowed) {
        entry->tokens -= UTP_NTRS_SOURCE_RATE_TOKEN_SCALE;
    }
    (void)pthread_mutex_unlock(&shard->lock);
    return allowed;
}

static bool utp_ntrs_write_probe_endpoint(uint8_t** cursor, size_t* remaining, const utp_ntrs_endpoint_t* endpoint)
{
    const size_t  address_length = endpoint->family == (uint8_t)AF_INET ? 4u : 16u;
    const uint8_t family         = endpoint->family == (uint8_t)AF_INET ? 4u : 6u;

    if (endpoint->port == 0u || (endpoint->family != (uint8_t)AF_INET && endpoint->family != (uint8_t)AF_INET6) ||
        *remaining < address_length + 4u) {
        return false;
    }
    (*cursor)[0] = family;
    (*cursor)[1] = 0u;
    utp_ntrs_write_u16(*cursor + 2u, endpoint->port);
    (void)memcpy(*cursor + 4u, endpoint->address, address_length);
    *cursor    += address_length + 4u;
    *remaining -= address_length + 4u;
    return true;
}

static bool utp_ntrs_write_probe_tlv(uint8_t** cursor, size_t* remaining, uint16_t type, const uint8_t* value,
                                     uint16_t value_length)
{
    if (*remaining < (size_t)value_length + 4u) {
        return false;
    }
    utp_ntrs_write_u16(*cursor, type);
    utp_ntrs_write_u16(*cursor + 2u, value_length);
    (void)memcpy(*cursor + 4u, value, value_length);
    *cursor    += (size_t)value_length + 4u;
    *remaining -= (size_t)value_length + 4u;
    return true;
}

static bool utp_ntrs_request_type_valid(uint8_t message_type, uint8_t phase)
{
    if (phase == UTP_NTRS_NAT_PHASE_PROBE1 || phase == UTP_NTRS_NAT_PHASE_PROBE2) {
        return message_type == UTP_NTRS_NAT_MESSAGE_PROBE_REQ;
    }
    return (phase == UTP_NTRS_NAT_PHASE_CHANGE_PORT || phase == UTP_NTRS_NAT_PHASE_CHANGE_IP) &&
           message_type == UTP_NTRS_NAT_MESSAGE_FILTER_REQ;
}

static bool utp_ntrs_parse_probe_request(const uint8_t* request, size_t request_length,
                                         utp_ntrs_probe_request_t* decoded)
{
    utp_packet_header_t header;
    const uint8_t*      payload;
    const uint8_t*      token = NULL;
    size_t              offset;
    bool                has_padding = false;

    if (request_length != UTP_NTRS_NAT_PACKET_SIZE ||
        utp_proto_decode_header(&header, request, request_length) != UTP_INTERNAL_ERROR_OK || header.scid != 0u ||
        header.dcid != 0u || header.packet_number == 0u || header.type != UTP_NTRS_UTP_PACKET_TYPE_NAT_PROBE ||
        header.reserve != 0u || (size_t)header.payload_length + UTP_PACKET_HEADER_SIZE != request_length) {
        return false;
    }
    payload = request + UTP_PACKET_HEADER_SIZE;
    if (header.payload_length < 4u || payload[0] != UTP_NTRS_NAT_PROBE_VERSION || payload[3] != 0u ||
        !utp_ntrs_request_type_valid(payload[1], payload[2])) {
        return false;
    }
    for (offset = 4u; offset < header.payload_length;) {
        const uint16_t type   = utp_ntrs_read_u16(payload + offset);
        const uint16_t length = utp_ntrs_read_u16(payload + offset + 2u);
        const uint8_t* value;
        size_t         index;

        offset += 4u;
        if (offset > header.payload_length || (size_t)length > (size_t)header.payload_length - offset) {
            return false;
        }
        value   = payload + offset;
        offset += length;
        if (type == UTP_NTRS_NAT_TLV_PROBE_TOKEN && token == NULL && length == UTP_NTRS_NAT_PROBE_TOKEN_SIZE) {
            token = value;
        } else if (type == UTP_NTRS_NAT_TLV_PADDING && !has_padding) {
            for (index = 0u; index < length; ++index) {
                if (value[index] != 0u) {
                    return false;
                }
            }
            has_padding = true;
        } else {
            return false;
        }
    }
    if (token == NULL || !has_padding) {
        return false;
    }
    *decoded = (utp_ntrs_probe_request_t){
        .token         = token,
        .packet_number = header.packet_number,
        .phase         = payload[2],
    };
    return true;
}

static bool utp_ntrs_write_mapped_and_origin(uint8_t** cursor, size_t* remaining,
                                             const utp_ntrs_endpoint_t* client_endpoint,
                                             const utp_ntrs_endpoint_t* origin)
{
    uint8_t* length_cursor;
    size_t   before;

    if (*remaining < 4u) {
        return false;
    }
    utp_ntrs_write_u16(*cursor, UTP_NTRS_NAT_TLV_MAPPED_ADDR);
    *cursor       += 2u;
    *remaining    -= 2u;
    length_cursor  = *cursor;
    before         = *remaining - 2u;
    *cursor       += 2u;
    *remaining    -= 2u;
    if (!utp_ntrs_write_probe_endpoint(cursor, remaining, client_endpoint)) {
        return false;
    }
    utp_ntrs_write_u16(length_cursor, (uint16_t)(before - *remaining));
    if (*remaining < 4u) {
        return false;
    }
    utp_ntrs_write_u16(*cursor, UTP_NTRS_NAT_TLV_ORIGIN_ADDR);
    *cursor       += 2u;
    *remaining    -= 2u;
    length_cursor  = *cursor;
    before         = *remaining - 2u;
    *cursor       += 2u;
    *remaining    -= 2u;
    if (!utp_ntrs_write_probe_endpoint(cursor, remaining, origin)) {
        return false;
    }
    utp_ntrs_write_u16(length_cursor, (uint16_t)(before - *remaining));
    return true;
}

static bool utp_ntrs_build_filter_response(const utp_ntrs_forward_filter_response_t* forward,
                                           const utp_ntrs_endpoint_t*                origin,
                                           uint8_t response[UTP_NTRS_NAT_PACKET_SIZE], size_t* response_length)
{
    const utp_packet_header_t header = {
        .packet_number = forward->packet_number,
        .type          = UTP_NTRS_UTP_PACKET_TYPE_NAT_PROBE,
    };
    utp_packet_header_t encoded_header = header;
    uint8_t*            cursor         = response + UTP_PACKET_HEADER_SIZE;
    size_t              remaining      = UTP_NTRS_NAT_PACKET_SIZE - UTP_PACKET_HEADER_SIZE;

    cursor[0]  = UTP_NTRS_NAT_PROBE_VERSION;
    cursor[1]  = UTP_NTRS_NAT_MESSAGE_FILTER_RSP;
    cursor[2]  = forward->phase;
    cursor[3]  = 0u;
    cursor    += 4u;
    remaining -= 4u;
    if (!utp_ntrs_write_probe_tlv(&cursor, &remaining, UTP_NTRS_NAT_TLV_PROBE_TOKEN, forward->token,
                                  UTP_NTRS_NAT_PROBE_TOKEN_SIZE) ||
        !utp_ntrs_write_mapped_and_origin(&cursor, &remaining, &forward->client, origin)) {
        return false;
    }
    *response_length              = (size_t)(cursor - response);
    encoded_header.payload_length = (uint16_t)(*response_length - UTP_PACKET_HEADER_SIZE);
    return utp_proto_encode_header(response, UTP_NTRS_NAT_PACKET_SIZE, &encoded_header) == UTP_INTERNAL_ERROR_OK;
}

bool utp_ntrs_udp_handle_probe_request(const uint8_t* request, size_t request_length,
                                       const utp_ntrs_endpoint_t* client_endpoint,
                                       const utp_ntrs_endpoint_t* probe_endpoint,
                                       const utp_ntrs_endpoint_t* change_port_endpoint,
                                       const utp_ntrs_endpoint_t* alternate_probe_endpoint, uint8_t* response,
                                       size_t response_capacity, size_t* response_length,
                                       utp_ntrs_udp_reply_socket_t* reply_socket)
{
    utp_packet_header_t      response_header;
    utp_ntrs_probe_request_t decoded;
    uint8_t                  response_type;
    uint8_t*                 cursor;
    size_t                   remaining;
    utp_ntrs_endpoint_t      origin;

    if (!utp_ntrs_parse_probe_request(request, request_length, &decoded) ||
        decoded.phase == UTP_NTRS_NAT_PHASE_CHANGE_IP) {
        return false;
    }
    *reply_socket =
        decoded.phase == UTP_NTRS_NAT_PHASE_CHANGE_PORT ? UTP_NTRS_UDP_REPLY_CHANGE_PORT : UTP_NTRS_UDP_REPLY_PROBE;
    origin        = *reply_socket == UTP_NTRS_UDP_REPLY_CHANGE_PORT ? *change_port_endpoint : *probe_endpoint;
    response_type = decoded.phase == UTP_NTRS_NAT_PHASE_PROBE1 || decoded.phase == UTP_NTRS_NAT_PHASE_PROBE2
                        ? UTP_NTRS_NAT_MESSAGE_PROBE_RSP
                        : UTP_NTRS_NAT_MESSAGE_FILTER_RSP;
    if (response_capacity < UTP_PACKET_HEADER_SIZE + 4u) {
        return false;
    }
    cursor     = response + UTP_PACKET_HEADER_SIZE;
    remaining  = response_capacity - UTP_PACKET_HEADER_SIZE;
    cursor[0]  = UTP_NTRS_NAT_PROBE_VERSION;
    cursor[1]  = response_type;
    cursor[2]  = decoded.phase;
    cursor[3]  = 0u;
    cursor    += 4u;
    remaining -= 4u;
    if (!utp_ntrs_write_probe_tlv(&cursor, &remaining, UTP_NTRS_NAT_TLV_PROBE_TOKEN, decoded.token,
                                  UTP_NTRS_NAT_PROBE_TOKEN_SIZE)) {
        return false;
    }
    if (!utp_ntrs_write_mapped_and_origin(&cursor, &remaining, client_endpoint, &origin)) {
        return false;
    }
    if (decoded.phase == UTP_NTRS_NAT_PHASE_PROBE1 && alternate_probe_endpoint->family == probe_endpoint->family &&
        !utp_ntrs_endpoint_same_ip(alternate_probe_endpoint, probe_endpoint)) {
        if (remaining < 4u) {
            return false;
        }
        utp_ntrs_write_u16(cursor, UTP_NTRS_NAT_TLV_ALTERNATE_ENDPOINT);
        cursor    += 2u;
        remaining -= 2u;
        {
            uint8_t* const length_cursor = cursor;
            const size_t   before        = remaining - 2u;

            cursor    += 2u;
            remaining -= 2u;
            if (!utp_ntrs_write_probe_endpoint(&cursor, &remaining, alternate_probe_endpoint)) {
                return false;
            }
            utp_ntrs_write_u16(length_cursor, (uint16_t)(before - remaining));
        }
    }
    *response_length = (size_t)(cursor - response);
    if (*response_length > request_length || *response_length - UTP_PACKET_HEADER_SIZE > UINT16_MAX) {
        return false;
    }
    response_header = (utp_packet_header_t){
        .packet_number  = decoded.packet_number,
        .payload_length = (uint16_t)(*response_length - UTP_PACKET_HEADER_SIZE),
        .type           = UTP_NTRS_UTP_PACKET_TYPE_NAT_PROBE,
    };
    return utp_proto_encode_header(response, response_capacity, &response_header) == UTP_INTERNAL_ERROR_OK;
}

static int32_t utp_ntrs_udp_create_socket(const utp_ntrs_endpoint_t* endpoint, const char* interface_name)
{
    struct sockaddr_storage address;
    socklen_t               address_length;
    const int32_t           family  = endpoint->family;
    const int32_t           fd      = socket(family, SOCK_DGRAM, 0);
    const int32_t           enabled = 1;

    if (fd < 0 || endpoint->port == 0u || !utp_ntrs_endpoint_to_sockaddr(endpoint, &address, &address_length) ||
        !utp_ntrs_socket_bind_interface(fd, interface_name) ||
        (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, (socklen_t)sizeof(enabled)) != 0) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, (socklen_t)sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, (socklen_t)sizeof(enabled)) != 0 ||
        bind(fd, (const struct sockaddr*)&address, address_length) != 0 || evutil_make_socket_nonblocking(fd) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return -1;
    }
    return fd;
}

static uint64_t utp_ntrs_udp_now_ms(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool utp_ntrs_endpoint_equal(const utp_ntrs_endpoint_t* left, const utp_ntrs_endpoint_t* right)
{
    return left->port == right->port && utp_ntrs_endpoint_same_ip(left, right);
}

static uint64_t utp_ntrs_forward_id(const utp_ntrs_probe_request_t* request, const utp_ntrs_endpoint_t* client)
{
    uint64_t     value          = UINT64_C(1469598103934665603);
    const size_t address_length = client->family == (uint8_t)AF_INET ? 4u : 16u;
    uint8_t      index;

    value ^= client->family;
    value *= UINT64_C(1099511628211);
    value ^= (uint8_t)(client->port >> 8u);
    value *= UINT64_C(1099511628211);
    value ^= (uint8_t)client->port;
    value *= UINT64_C(1099511628211);
    for (index = 0u; index < address_length; ++index) {
        value ^= client->address[index];
        value *= UINT64_C(1099511628211);
    }
    for (index = 0u; index < UTP_NTRS_NAT_PROBE_TOKEN_SIZE; ++index) {
        value ^= request->token[index];
        value *= UINT64_C(1099511628211);
    }
    for (index = 0u; index < sizeof(request->packet_number); ++index) {
        value ^= (uint8_t)(request->packet_number >> (index * 8u));
        value *= UINT64_C(1099511628211);
    }
    return value == 0u ? UINT64_C(1) : value;
}

static void utp_ntrs_affinity_store(utp_ntrs_udp_worker_t* worker, const utp_ntrs_endpoint_t* client,
                                    const utp_ntrs_node_instance_t* primary, const utp_ntrs_endpoint_t* primary_probe,
                                    uint64_t now_ms)
{
    utp_ntrs_udp_affinity_t* slot = NULL;
    uint32_t                 index;

    for (index = 0u; index < UTP_NTRS_AFFINITY_CAPACITY; ++index) {
        utp_ntrs_udp_affinity_t* const current = &worker->affinities[index];

        if (current->expires_at_ms <= now_ms) {
            slot = current;
            break;
        }
        if (utp_ntrs_endpoint_equal(&current->client, client)) {
            slot = current;
            break;
        }
        if (slot == NULL || current->expires_at_ms < slot->expires_at_ms) {
            slot = current;
        }
    }
    *slot = (utp_ntrs_udp_affinity_t){
        .client        = *client,
        .primary_probe = *primary_probe,
        .primary       = *primary,
        .expires_at_ms = now_ms + UTP_NTRS_AFFINITY_LIFETIME_MS,
    };
}

static const utp_ntrs_udp_affinity_t* utp_ntrs_affinity_find(const utp_ntrs_udp_worker_t* worker,
                                                             const utp_ntrs_endpoint_t* client, uint64_t now_ms)
{
    uint32_t index;

    for (index = 0u; index < UTP_NTRS_AFFINITY_CAPACITY; ++index) {
        const utp_ntrs_udp_affinity_t* const current = &worker->affinities[index];

        if (current->expires_at_ms > now_ms && utp_ntrs_endpoint_equal(&current->client, client)) {
            return current;
        }
    }
    return NULL;
}

static void utp_ntrs_udp_on_forward_event(evutil_socket_t fd, int16_t events, void* user_data)
{
    utp_ntrs_udp_server_t* const server = user_data;
    uint8_t                      ignored[64];

    (void)events;
    while (recv(fd, ignored, sizeof(ignored), 0) > 0) {
    }
    for (;;) {
        utp_ntrs_udp_forward_item_t* item;

        (void)pthread_mutex_lock(&server->forward_lock);
        item = server->forward_head;
        if (item != NULL) {
            server->forward_head = item->next;
            if (server->forward_head == NULL) {
                server->forward_tail = NULL;
            }
        }
        (void)pthread_mutex_unlock(&server->forward_lock);
        if (item == NULL) {
            return;
        }
        server->on_forward(server->forward_user_data, &item->forward);
        free(item);
    }
}

static void utp_ntrs_udp_enqueue_forward(utp_ntrs_udp_server_t*                    server,
                                         const utp_ntrs_forward_filter_response_t* forward)
{
    utp_ntrs_udp_forward_item_t* const item   = malloc(sizeof(*item));
    const uint8_t                      notify = 1u;

    if (item == NULL || server->forward_write_fd < 0) {
        free(item);
        return;
    }
    item->next    = NULL;
    item->forward = *forward;
    (void)pthread_mutex_lock(&server->forward_lock);
    if (server->forward_tail != NULL) {
        server->forward_tail->next = item;
    } else {
        server->forward_head = item;
    }
    server->forward_tail = item;
    (void)pthread_mutex_unlock(&server->forward_lock);
    (void)send(server->forward_write_fd, &notify, sizeof(notify), MSG_DONTWAIT);
}

static void utp_ntrs_udp_on_read(evutil_socket_t fd, short events, void* user_data)
{
    const utp_ntrs_udp_socket_context_t* const context = user_data;
    utp_ntrs_udp_worker_t* const               worker  = context->worker;
    uint32_t                                   index;

    (void)events;
    for (index = 0u; index < UTP_NTRS_UDP_READ_BATCH; ++index) {
        struct sockaddr_storage     client_address;
        struct iovec                input   = {0};
        struct msghdr               message = {0};
        uint8_t                     request[UTP_NTRS_NAT_PACKET_SIZE];
        uint8_t                     response[UTP_NTRS_NAT_PACKET_SIZE];
        utp_ntrs_endpoint_t         client_endpoint;
        utp_ntrs_endpoint_t         alternate_endpoint;
        utp_ntrs_node_instance_t    primary_instance;
        utp_ntrs_probe_request_t    decoded;
        utp_ntrs_udp_reply_socket_t reply_socket;
        size_t                      response_length;
        ssize_t                     received;

        input.iov_base      = request;
        input.iov_len       = sizeof(request);
        message.msg_name    = &client_address;
        message.msg_namelen = (socklen_t)sizeof(client_address);
        message.msg_iov     = &input;
        message.msg_iovlen  = 1u;
        received            = recvmsg(fd, &message, 0);

        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return;
            }
            break;
        }
        if ((message.msg_flags & MSG_TRUNC) != 0 || context->socket_kind != UTP_NTRS_UDP_REPLY_PROBE) {
            continue;
        }
        (void)pthread_rwlock_rdlock(&worker->server->alternate_lock);
        alternate_endpoint = worker->options.alternate_probe_endpoint;
        primary_instance   = worker->options.primary_instance;
        (void)pthread_rwlock_unlock(&worker->server->alternate_lock);
        if (!utp_ntrs_endpoint_from_sockaddr(&client_endpoint, (const struct sockaddr*)&client_address,
                                             message.msg_namelen) ||
            !utp_ntrs_source_rate_allows(worker->server, &client_endpoint, utp_ntrs_udp_now_ms()) ||
            !utp_ntrs_parse_probe_request(request, (size_t)received, &decoded)) {
            continue;
        }
        if (decoded.phase == UTP_NTRS_NAT_PHASE_CHANGE_IP) {
            const utp_ntrs_udp_affinity_t* const affinity =
                utp_ntrs_affinity_find(worker, &client_endpoint, utp_ntrs_udp_now_ms());

            if (affinity != NULL) {
                utp_ntrs_forward_filter_response_t forward = {
                    .target        = affinity->primary,
                    .client        = client_endpoint,
                    .forward_id    = utp_ntrs_forward_id(&decoded, &client_endpoint),
                    .packet_number = decoded.packet_number,
                    .phase         = decoded.phase,
                };

                (void)memcpy(forward.token, decoded.token, sizeof(forward.token));
                utp_ntrs_udp_enqueue_forward(worker->server, &forward);
            }
            continue;
        }
        if (!utp_ntrs_udp_handle_probe_request(request, (size_t)received, &client_endpoint,
                                               &worker->options.probe_endpoint, &worker->options.change_port_endpoint,
                                               &alternate_endpoint, response, sizeof(response), &response_length,
                                               &reply_socket)) {
            continue;
        }
        (void)sendto(reply_socket == UTP_NTRS_UDP_REPLY_CHANGE_PORT ? worker->change_port_fd : worker->probe_fd,
                     response, response_length, 0, (const struct sockaddr*)&client_address, message.msg_namelen);
        if (decoded.phase == UTP_NTRS_NAT_PHASE_PROBE1 &&
            alternate_endpoint.family == worker->options.probe_endpoint.family &&
            !utp_ntrs_endpoint_same_ip(&alternate_endpoint, &worker->options.probe_endpoint)) {
            utp_ntrs_affinity_store(worker, &client_endpoint, &primary_instance, &alternate_endpoint,
                                    utp_ntrs_udp_now_ms());
        }
    }
}

static void* utp_ntrs_udp_worker_main(void* user_data)
{
    utp_ntrs_udp_worker_t* const worker = user_data;

    (void)event_base_dispatch(worker->base);
    return NULL;
}

static void utp_ntrs_udp_worker_destroy(utp_ntrs_udp_worker_t* worker)
{
    if (worker->probe_event != NULL) {
        event_free(worker->probe_event);
    }
    if (worker->change_port_event != NULL) {
        event_free(worker->change_port_event);
    }
    if (worker->probe_fd >= 0) {
        (void)close(worker->probe_fd);
    }
    if (worker->change_port_fd >= 0) {
        (void)close(worker->change_port_fd);
    }
    if (worker->base != NULL) {
        event_base_free(worker->base);
    }
    free(worker->affinities);
    *worker = (utp_ntrs_udp_worker_t){
        .probe_fd       = -1,
        .change_port_fd = -1,
    };
}

utp_ntrs_udp_server_t* utp_ntrs_udp_server_start(const utp_ntrs_udp_server_options_t* options)
{
    utp_ntrs_udp_server_t* server;
    const uint16_t         worker_count = options->worker_count == 0u ? 1u : options->worker_count;
    uint16_t               index;

    if (pthread_once(&utp_ntrs_event_thread_once, utp_ntrs_enable_event_threads) != 0 ||
        utp_ntrs_event_thread_status != 0 || options->probe_endpoint.family != options->change_port_endpoint.family ||
        options->probe_endpoint.port == 0u || options->change_port_endpoint.port == 0u ||
        options->probe_endpoint.port == options->change_port_endpoint.port ||
        !utp_ntrs_endpoint_same_ip(&options->probe_endpoint, &options->change_port_endpoint)) {
        return NULL;
    }
    server = calloc(1u, sizeof(*server));
    if (server == NULL) {
        return NULL;
    }
    server->forward_read_fd  = -1;
    server->forward_write_fd = -1;
    server->workers          = calloc(worker_count, sizeof(*server->workers));
    if (server->workers == NULL) {
        free(server);
        return NULL;
    }
    server->worker_count = worker_count;
    server->source_rate_per_second =
        options->source_rate_per_second == 0u ? UTP_NTRS_UDP_DEFAULT_SOURCE_RATE : options->source_rate_per_second;
    server->source_burst = options->source_burst == 0u ? UTP_NTRS_UDP_DEFAULT_SOURCE_BURST : options->source_burst;
    if (server->source_rate_per_second > UTP_NTRS_SOURCE_RATE_MAX_PER_SECOND ||
        server->source_burst > UTP_NTRS_SOURCE_RATE_MAX_BURST) {
        free(server->workers);
        free(server);
        return NULL;
    }
    for (index = 0u; index < worker_count; ++index) {
        server->workers[index] = (utp_ntrs_udp_worker_t){
            .probe_fd       = -1,
            .change_port_fd = -1,
        };
    }
    if (pthread_rwlock_init(&server->alternate_lock, NULL) != 0) {
        free(server->workers);
        free(server);
        return NULL;
    }
    if (pthread_mutex_init(&server->forward_lock, NULL) != 0) {
        (void)pthread_rwlock_destroy(&server->alternate_lock);
        free(server->workers);
        free(server);
        return NULL;
    }
    for (index = 0u; index < UTP_NTRS_SOURCE_RATE_SHARD_COUNT; ++index) {
        if (pthread_mutex_init(&server->source_rate_shards[index].lock, NULL) != 0) {
            while (index != 0u) {
                --index;
                (void)pthread_mutex_destroy(&server->source_rate_shards[index].lock);
            }
            (void)pthread_mutex_destroy(&server->forward_lock);
            (void)pthread_rwlock_destroy(&server->alternate_lock);
            free(server->workers);
            free(server);
            return NULL;
        }
    }
    server->on_forward        = options->on_forward;
    server->forward_user_data = options->user_data;
    if (options->on_forward != NULL) {
        int32_t notify_sockets[2] = {-1, -1};

        if (options->main_base == NULL ||
            socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, notify_sockets) != 0 ||
            (server->forward_read_fd = notify_sockets[0]) < 0 || (server->forward_write_fd = notify_sockets[1]) < 0 ||
            (server->forward_event = event_new(options->main_base, server->forward_read_fd, EV_READ | EV_PERSIST,
                                               utp_ntrs_udp_on_forward_event, server)) == NULL ||
            event_add(server->forward_event, NULL) != 0) {
            utp_ntrs_udp_server_stop(server);
            return NULL;
        }
    }
    for (index = 0u; index < worker_count; ++index) {
        utp_ntrs_udp_worker_t* const worker = &server->workers[index];

        *worker = (utp_ntrs_udp_worker_t){
            .server         = server,
            .options        = *options,
            .probe_fd       = -1,
            .change_port_fd = -1,
        };
        worker->base           = event_base_new();
        worker->affinities     = calloc(UTP_NTRS_AFFINITY_CAPACITY, sizeof(*worker->affinities));
        worker->probe_fd       = utp_ntrs_udp_create_socket(&options->probe_endpoint, options->interface_name);
        worker->change_port_fd = utp_ntrs_udp_create_socket(&options->change_port_endpoint, options->interface_name);
        worker->probe_context  = (utp_ntrs_udp_socket_context_t){
            .worker      = worker,
            .socket_kind = UTP_NTRS_UDP_REPLY_PROBE,
        };
        worker->change_port_context = (utp_ntrs_udp_socket_context_t){
            .worker      = worker,
            .socket_kind = UTP_NTRS_UDP_REPLY_CHANGE_PORT,
        };
        if (worker->base == NULL || worker->affinities == NULL || worker->probe_fd < 0 || worker->change_port_fd < 0 ||
            (worker->probe_event = event_new(worker->base, worker->probe_fd, EV_READ | EV_PERSIST, utp_ntrs_udp_on_read,
                                             &worker->probe_context)) == NULL ||
            (worker->change_port_event = event_new(worker->base, worker->change_port_fd, EV_READ | EV_PERSIST,
                                                   utp_ntrs_udp_on_read, &worker->change_port_context)) == NULL ||
            event_add(worker->probe_event, NULL) != 0 || event_add(worker->change_port_event, NULL) != 0) {
            utp_ntrs_udp_server_stop(server);
            return NULL;
        }
    }
    for (index = 0u; index < worker_count; ++index) {
        if (pthread_create(&server->workers[index].thread, NULL, utp_ntrs_udp_worker_main, &server->workers[index]) !=
            0) {
            utp_ntrs_udp_server_stop(server);
            return NULL;
        }
        server->workers[index].started = true;
    }
    return server;
}

void utp_ntrs_udp_server_stop(utp_ntrs_udp_server_t* server)
{
    uint16_t index;

    if (server == NULL) {
        return;
    }
    for (index = 0u; index < server->worker_count; ++index) {
        utp_ntrs_udp_worker_t* const worker = &server->workers[index];

        if (worker->started) {
            (void)event_base_loopbreak(worker->base);
        }
    }
    for (index = 0u; index < server->worker_count; ++index) {
        utp_ntrs_udp_worker_t* const worker = &server->workers[index];

        if (worker->started) {
            (void)pthread_join(worker->thread, NULL);
        }
        utp_ntrs_udp_worker_destroy(worker);
    }
    if (server->forward_event != NULL) {
        event_free(server->forward_event);
    }
    if (server->forward_read_fd >= 0) {
        (void)close(server->forward_read_fd);
    }
    if (server->forward_write_fd >= 0) {
        (void)close(server->forward_write_fd);
    }
    while (server->forward_head != NULL) {
        utp_ntrs_udp_forward_item_t* const item = server->forward_head;

        server->forward_head = item->next;
        free(item);
    }
    free(server->workers);
    (void)pthread_mutex_destroy(&server->forward_lock);
    for (index = 0u; index < UTP_NTRS_SOURCE_RATE_SHARD_COUNT; ++index) {
        (void)pthread_mutex_destroy(&server->source_rate_shards[index].lock);
    }
    (void)pthread_rwlock_destroy(&server->alternate_lock);
    free(server);
}

void utp_ntrs_udp_server_set_primary(utp_ntrs_udp_server_t* server, const utp_ntrs_node_instance_t* primary_instance,
                                     const utp_ntrs_endpoint_t* primary_endpoint)
{
    uint16_t index;

    (void)pthread_rwlock_wrlock(&server->alternate_lock);
    for (index = 0u; index < server->worker_count; ++index) {
        server->workers[index].options.primary_instance =
            primary_instance == NULL ? (utp_ntrs_node_instance_t){0} : *primary_instance;
        server->workers[index].options.alternate_probe_endpoint =
            primary_endpoint == NULL ? (utp_ntrs_endpoint_t){0} : *primary_endpoint;
    }
    (void)pthread_rwlock_unlock(&server->alternate_lock);
}

bool utp_ntrs_udp_server_send_filter_response(utp_ntrs_udp_server_t*                    server,
                                              const utp_ntrs_forward_filter_response_t* forward)
{
    struct sockaddr_storage client_address;
    socklen_t               client_length;
    uint8_t                 response[UTP_NTRS_NAT_PACKET_SIZE];
    size_t                  response_length;

    if (server == NULL || forward == NULL || server->worker_count == 0u ||
        forward->client.family != server->workers[0].options.probe_endpoint.family ||
        !utp_ntrs_endpoint_to_sockaddr(&forward->client, &client_address, &client_length) ||
        !utp_ntrs_build_filter_response(forward, &server->workers[0].options.probe_endpoint, response,
                                        &response_length)) {
        return false;
    }
    return sendto(server->workers[0].probe_fd, response, response_length, 0, (const struct sockaddr*)&client_address,
                  client_length) == (ssize_t)response_length;
}
