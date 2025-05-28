#include "kcp_mtu.h"

#include <assert.h>

#include <event2/event.h>

#include <xxhash.h>

#include "kcp_error.h"
#include "kcp_time.h"
#include "kcp_net_utils.h"
#include "kcp_log.h"

#define GRE_HEADER_SIZE     24
#define PPPOE_HEADER_SIZE   8
#define MPPE_HEADER_SIZE    2
// packets have been observed in the wild that were fragmented
// with a payload of 1416 for the first fragment
// There are reports of routers that have MTU sizes as small as 1392
#define FUDGE_HEADER_SIZE       36

#define UDP_IPV4_OVERHEAD (IPV4_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_IPV6_OVERHEAD (IPV6_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_TEREDO_OVERHEAD (UDP_IPV4_OVERHEAD + UDP_IPV6_OVERHEAD)

#define UDP_IPV4_MTU (ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_IPV6_MTU (ETHERNET_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_TEREDO_MTU (ETHERNET_MTU_V6_MIN - IPV6_HEADER_SIZE - UDP_HEADER_SIZE)

int32_t kcp_get_min_mtu(bool ipv6)
{
    if (ipv6) {
        return ETHERNET_MTU_V6_MIN - IPV6_HEADER_SIZE - UDP_HEADER_SIZE;
    } else {
        return ETHERNET_MTU_V4_MIN - IPV4_HEADER_SIZE - UDP_HEADER_SIZE;
    }
}

int32_t kcp_get_mtu(bool ipv6)
{
    if (ipv6) {
        return UDP_IPV6_MTU;
    } else {
        return UDP_IPV4_MTU;
    }
}

int32_t kcp_get_localhost_mss(bool ipv6)
{
    if (ipv6) {
        return LOCALHOST_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE;
    } else {
        return LOCALHOST_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE;
    }
}

static void mtu_probe_update(kcp_connection_t *kcp_conn);

static void kcp_send_mtu_probe_packet(kcp_connection_t *kcp_conn)
{
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;

    do {
        int32_t mtu_current = (probe_ctx->mtu_lbound + probe_ctx->mtu_ubound) / 2;
        bool ipv6 = kcp_conn->remote_host.sa.sa_family == AF_INET6;
        int32_t data_length = 0;
        if (ipv6) {
            data_length = mtu_current - IPV6_HEADER_SIZE - UDP_HEADER_SIZE;
        } else {
            data_length = mtu_current - IPV4_HEADER_SIZE - UDP_HEADER_SIZE;
        }

        kcp_proto_header_t header;
        kcp_proto_header_t *kcp_header = &header;
        header.conv = kcp_conn->conv;
        header.cmd = KCP_CMD_MTU_PROBE;
        header.opt = 0;
        header.frg = 0;
        header.wnd = 0;
        header.packet_data.ts = kcp_time_monotonic_us();
        header.packet_data.sn = header.packet_data.ts;
        probe_ctx->prev_sn = header.packet_data.sn;
        header.packet_data.una = 0;
        header.packet_data.len = data_length;
        header.packet_data.data = probe_ctx->probe_buf + KCP_HEADER_SIZE;

        probe_ctx->hash = XXH64(header.packet_data.data, header.packet_data.len, 0);

        char *buffer_offset = probe_ctx->probe_buf;
        *(uint32_t *)buffer_offset = htole32(kcp_header->conv);
        buffer_offset += 4;
        *(uint8_t *)buffer_offset = kcp_header->cmd;
        buffer_offset += 1;
        *(uint8_t *)buffer_offset = kcp_header->frg;
        buffer_offset += 1;
        *(uint16_t *)buffer_offset = htole16(kcp_header->wnd);
        buffer_offset += 2;
        *(uint32_t *)buffer_offset = htole64(kcp_header->packet_data.ts);
        buffer_offset += 8;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.psn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.una);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.len);
        buffer_offset += 4;

        struct iovec iov[3];
        int32_t count = kcp_conn->mtu_probe_ctx->retries < 2 ? kcp_conn->mtu_probe_ctx->retries : 1;
        for (int32_t i = 0; i < count; ++i) {
            iov[i].iov_base = probe_ctx->probe_buf;
            iov[i].iov_len = KCP_HEADER_SIZE + data_length;
        }

        int32_t status = kcp_send_packet(kcp_conn, iov, count);
        if (status == count) {
            break;
        }

        if (get_last_errno() == EMSGSIZE) {
            probe_ctx->mtu_ubound = mtu_current - 1;
        } else {
            KCP_LOGE("send packet error. [%d, %s]", errno, strerror(errno));
            break;
        }
    } while (true);
}

static void kcp_mtu_probe_timeout_cb(evutil_socket_t fd, short event, void *arg)
{
    kcp_connection_t *kcp_conn = (kcp_connection_t *)arg;
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    if (probe_ctx == NULL) {
        return;
    }

    --probe_ctx->mtu_ubound;
    mtu_probe_update(kcp_conn);
}

int32_t kcp_mtu_probe(kcp_connection_t *kcp_conn, uint32_t timeout, uint16_t retry)
{
    if (timeout == 0) {
        timeout = 2 * kcp_conn->rx_srtt;
    }

    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    kcp_conn->mtu_probe_ctx = probe_ctx;
    probe_ctx->mtu_lbound = probe_ctx->mtu_current; // 下一次探测从上一次的MTU最小值开始
    probe_ctx->mtu_ubound = ETHERNET_MTU;
    probe_ctx->timeout = timeout;
    probe_ctx->retries = retry;
    if (probe_ctx->probe_timeout_event == NULL) {
        probe_ctx->probe_timeout_event = evtimer_new(kcp_conn->kcp_ctx->event_loop, kcp_mtu_probe_timeout_cb, kcp_conn);
    }
    if (probe_ctx->probe_timeout_event == NULL) {
        return NO_MEMORY;
    }
    evtimer_del(probe_ctx->probe_timeout_event);

    kcp_send_mtu_probe_packet(kcp_conn);

    struct timeval tv = {timeout / 1000, (timeout % 1000) * 1000};
    return evtimer_add(probe_ctx->probe_timeout_event, &tv);
}

int32_t kcp_mtu_probe_received(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    uint64_t hash = 0;
    kcp_proto_header_t mtu_ack_header;
    mtu_ack_header.conv = kcp_header->conv;
    mtu_ack_header.cmd = KCP_CMD_MTU_ACK;
    mtu_ack_header.opt = 0;
    mtu_ack_header.frg = 0;
    mtu_ack_header.wnd = 0;
    mtu_ack_header.packet_data.ts = timestamp;
    mtu_ack_header.packet_data.sn = kcp_header->packet_data.sn;
    mtu_ack_header.packet_data.psn = 0;
    mtu_ack_header.packet_data.una = 0;
    mtu_ack_header.packet_data.len = sizeof(uint64_t);
    mtu_ack_header.packet_data.data = (char *)&hash;
    hash = XXH64(kcp_header->packet_data.data, kcp_header->packet_data.len, 0);
    hash = htole64(hash);

    char buffer[KCP_HEADER_SIZE + sizeof(uint64_t)];
    kcp_proto_header_encode(&mtu_ack_header, buffer, sizeof(buffer));
    struct iovec iov[1];
    for (int32_t i = 0; i < 1; ++i) {
        iov[i].iov_base = buffer;
        iov[i].iov_len = sizeof(buffer);
    }

    kcp_send_packet(kcp_conn, iov, 3);
}

int32_t kcp_mtu_ack_received(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    if (kcp_header->packet_data.sn != probe_ctx->prev_sn) {
        return NO_ERROR;
    }

    uint64_t hash = le64toh(*(uint64_t *)kcp_header->packet_data.data);
    if (hash != probe_ctx->hash) {
        return NO_ERROR;
    }

    if (probe_ctx->mtu_lbound >= probe_ctx->mtu_ubound) {
        kcp_conn->mtu_probe_ctx->on_probe_completed(kcp_conn, probe_ctx->mtu_current, NO_ERROR);
        return NO_ERROR;
    }

    probe_ctx->mtu_lbound = probe_ctx->mtu_current + 1;
    mtu_probe_update(kcp_conn);
}

/////////ICMP/////////
static kcp_connection_t *parse_icmp_payload(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr)
{
    if (len < KCP_HEADER_SIZE) {
        return NULL;
    }
    uint32_t conv = le32toh(*(uint32_t *)buffer); // 会话ID
    if (!(conv & KCP_CONV_FLAG)) {
        return NULL;
    }

    kcp_connection_t* kcp_conn = connection_set_search(&kcp_ctx->connection_set, conv);
    if (kcp_conn != NULL) {
        if (sockaddr_equal(&kcp_conn->remote_host, remote_addr)) {
            return NULL;
        }

        return kcp_conn;
    }

    return NULL;
}

static void mtu_probe_update(kcp_connection_t *kcp_conn)
{
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    assert(probe_ctx->mtu_lbound <= probe_ctx->mtu_ubound);

    // 二分法
    probe_ctx->mtu_current = (probe_ctx->mtu_lbound + probe_ctx->mtu_ubound) / 2;

    // 如果二者相近, 则认为找到了最佳的MTU
    if (probe_ctx->mtu_ubound - probe_ctx->mtu_lbound <= 16) {
        probe_ctx->mtu_current = probe_ctx->mtu_lbound;
        probe_ctx->mtu_ubound = probe_ctx->mtu_lbound;

        // 结束探测
        event_del(probe_ctx->probe_timeout_event);

        if (kcp_conn->mtu_probe_ctx->on_probe_completed != NULL) {
            kcp_conn->mtu_probe_ctx->on_probe_completed(kcp_conn, probe_ctx->mtu_current, NO_ERROR);
        }
    } else {
        kcp_mtu_probe(kcp_conn, probe_ctx->timeout, probe_ctx->retries);
    }
}

void kcp_process_icmp_fragmentation(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr, uint16_t next_hop_mtu)
{
    kcp_connection_t *kcp_conn = parse_icmp_payload(kcp_ctx, buffer, len, remote_addr);
    if (kcp_conn == NULL) {
        return;
    }

    if (next_hop_mtu >= 576 && next_hop_mtu < 0x2000) {
        kcp_conn->mtu_probe_ctx->mtu_ubound = MIN(next_hop_mtu, kcp_conn->mtu_probe_ctx->mtu_ubound);
        mtu_probe_update(kcp_conn);
        // this is something of a speecial case, where we don't set mtu_current
        // to the value in between the floor and the ceiling. We can update the
        // floor, because there might be more network segments after the one
        // that sent this ICMP with smaller MTUs. But we want to test this
        // MTU size first. If the next probe gets through, mtu_floor is updated
        kcp_conn->mtu_probe_ctx->mtu_current = kcp_conn->mtu_probe_ctx->mtu_ubound;
    } else {
        // Otherwise, binary search. At this point we don't actually know
        // what size the packet that failed was, and apparently we can't
        // trust the next hop mtu either. It seems reasonably conservative
        // to just lower the ceiling. This should not happen on working networks
        // anyway.
        kcp_conn->mtu_probe_ctx->mtu_ubound = (kcp_conn->mtu_probe_ctx->mtu_lbound + kcp_conn->mtu_probe_ctx->mtu_ubound) / 2;
        mtu_probe_update(kcp_conn);
    }
}

void kcp_process_icmp_unreach(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr)
{
    kcp_connection_t *kcp_conn = parse_icmp_payload(kcp_ctx, buffer, len, remote_addr);
    if (kcp_conn == NULL) {
        return;
    }

    kcp_conn->state = KCP_STATE_DISCONNECTED;
    kcp_conn->kcp_ctx->callback.on_error(kcp_ctx, kcp_conn, UDP_UNREACH);
}

void kcp_process_icmp_error(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr)
{
    kcp_connection_t *kcp_conn = parse_icmp_payload(kcp_ctx, buffer, len, remote_addr);
    if (kcp_conn == NULL) {
        return;
    }

    kcp_conn->state = KCP_STATE_DISCONNECTED;
    kcp_conn->kcp_ctx->callback.on_error(kcp_ctx, kcp_conn, ICMP_ERROR);
}
