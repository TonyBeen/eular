#include "context/send_control.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#include "proto/proto.h"
#include "util/allocator.h"

#define UTP_SEND_CONTROL_DEFAULT_REORDER_THRESHOLD   3u
#define UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US UINT64_C(60000000)
#define UTP_SEND_CONTROL_DEFAULT_RTO_US              UINT64_C(500000)
#define UTP_SEND_CONTROL_MIN_RTO_US                  UINT64_C(200000)
#define UTP_SEND_CONTROL_INITIAL_RTT_US              UINT64_C(333333)
#define UTP_SEND_CONTROL_MAX_RTO_BACKOFFS            10u
#define UTP_SEND_CONTROL_MAX_TLP_COUNT               2u
#define UTP_SEND_CONTROL_PACER_GRANULARITY_US        1000u
#define UTP_SEND_CONTROL_ATTEMPT_BLOCK_SIZE          32u

static utp_internal_error_t utp_send_control_attempt_add_block(utp_send_control_t* control)
{
    utp_send_attempt_block_t* block;
    size_t                    allocation_size;
    size_t                    index;

    assert(control != NULL);
    allocation_size = control->attempt_block_size * sizeof(*block->nodes);
    block           = utp_allocator_alloc(NULL, sizeof(*block));
    if (block == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    block->nodes = utp_allocator_alloc(NULL, allocation_size);
    if (block->nodes == NULL) {
        utp_allocator_free(NULL, block);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    block->next                = control->attempt_blocks;
    block->capacity            = control->attempt_block_size;
    control->attempt_blocks    = block;
    control->attempt_capacity += block->capacity;
    for (index = 0u; index < block->capacity; ++index) {
        block->nodes[index].free_next = control->attempt_free;
        control->attempt_free         = &block->nodes[index];
    }
    return UTP_INTERNAL_ERROR_OK;
}

static uint8_t utp_send_control_attempt_level(uint64_t packet_number)
{
    uint64_t value = packet_number ^ (packet_number >> 17u) ^ (packet_number >> 31u);
    uint8_t  level = 1u;

    while (level < UTP_SEND_ATTEMPT_MAX_LEVEL && (value & UINT64_C(3)) == 0u) {
        ++level;
        value >>= 2u;
    }
    return level;
}

static utp_send_attempt_node_t* utp_send_control_attempt_lower_bound(const utp_send_control_t* control,
                                                                     uint64_t                  packet_number)
{
    utp_send_attempt_node_t* node;
    uint8_t                  level;

    assert(control != NULL);
    node  = NULL;
    level = control->attempt_level;
    while (level != 0u) {
        --level;
        while ((node == NULL ? control->attempt_head[level] : node->next[level]) != NULL &&
               (node == NULL ? control->attempt_head[level] : node->next[level])->packet_number < packet_number) {
            node = node == NULL ? control->attempt_head[level] : node->next[level];
        }
    }
    return node == NULL ? control->attempt_head[0] : node->next[0];
}

static void utp_send_control_attempt_find_predecessors(
    const utp_send_control_t* control, uint64_t packet_number,
    utp_send_attempt_node_t* predecessors[UTP_SEND_ATTEMPT_MAX_LEVEL])
{
    utp_send_attempt_node_t* node;
    uint8_t                  level;
    uint8_t                  clear_index;

    assert(control != NULL);
    assert(predecessors != NULL);
    node = NULL;
    for (level = control->attempt_level; level != 0u; --level) {
        const uint8_t index = (uint8_t)(level - 1u);

        while ((node == NULL ? control->attempt_head[index] : node->next[index]) != NULL &&
               (node == NULL ? control->attempt_head[index] : node->next[index])->packet_number < packet_number) {
            node = node == NULL ? control->attempt_head[index] : node->next[index];
        }
        predecessors[index] = node;
    }
    for (clear_index = control->attempt_level; clear_index < UTP_SEND_ATTEMPT_MAX_LEVEL; ++clear_index) {
        predecessors[clear_index] = NULL;
    }
}

static void utp_send_control_attempt_unlink_index(utp_send_control_t* control, utp_send_attempt_node_t* target)
{
    utp_send_attempt_node_t* predecessors[UTP_SEND_ATTEMPT_MAX_LEVEL];
    uint8_t                  level;

    assert(control != NULL);
    assert(target != NULL);
    utp_send_control_attempt_find_predecessors(control, target->packet_number, predecessors);
    for (level = 0u; level < control->attempt_level; ++level) {
        utp_send_attempt_node_t** link =
            predecessors[level] == NULL ? &control->attempt_head[level] : &predecessors[level]->next[level];

        if (*link == target) {
            *link = target->next[level];
        }
    }
    while (control->attempt_level > 1u && control->attempt_head[control->attempt_level - 1u] == NULL) {
        --control->attempt_level;
    }
}

static void utp_send_control_attempt_release(utp_send_control_t* control, utp_send_attempt_node_t* node)
{
    utp_packet_out_t* packet;

    assert(control != NULL);
    assert(node != NULL);
    packet = node->packet;
    assert(packet != NULL);
    utp_send_control_attempt_unlink_index(control, node);
    if (node->packet_prev != NULL) {
        node->packet_prev->packet_next = node->packet_next;
    } else {
        packet->attempts = node->packet_next;
    }
    if (node->packet_next != NULL) {
        node->packet_next->packet_prev = node->packet_prev;
    }
    node->packet          = NULL;
    node->packet_next     = NULL;
    node->packet_prev     = NULL;
    node->free_next       = control->attempt_free;
    control->attempt_free = node;
    assert(control->attempt_count != 0u);
    assert(packet->attempt_count != 0u);
    --control->attempt_count;
    --packet->attempt_count;
}

static utp_internal_error_t utp_send_control_record_attempt(utp_send_control_t* control, utp_packet_out_t* packet)
{
    utp_send_attempt_node_t* predecessors[UTP_SEND_ATTEMPT_MAX_LEVEL];
    utp_send_attempt_node_t* node;
    uint8_t                  level;

    assert(control != NULL);
    assert(packet != NULL);
    assert(packet->packet_number != 0u);
    assert(packet->sent_time_us != 0u);
    if (control->attempt_free == NULL) {
        const utp_internal_error_t error = utp_send_control_attempt_add_block(control);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    utp_send_control_attempt_find_predecessors(control, packet->packet_number, predecessors);
    node = predecessors[0] == NULL ? control->attempt_head[0] : predecessors[0]->next[0];
    if (node != NULL && node->packet_number == packet->packet_number) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    node                  = control->attempt_free;
    control->attempt_free = node->free_next;
    node->packet          = packet;
    node->packet_number   = packet->packet_number;
    node->sent_time_us    = packet->sent_time_us;
    node->packet_prev     = NULL;
    node->packet_next     = packet->attempts;
    if (packet->attempts != NULL) {
        packet->attempts->packet_prev = node;
    }
    packet->attempts = node;
    ++packet->attempt_count;
    level = utp_send_control_attempt_level(node->packet_number);
    if (level > control->attempt_level) {
        while (control->attempt_level < level) {
            predecessors[control->attempt_level] = NULL;
            ++control->attempt_level;
        }
    }
    for (uint8_t index = 0u; index < level; ++index) {
        node->next[index] =
            predecessors[index] == NULL ? control->attempt_head[index] : predecessors[index]->next[index];
        if (predecessors[index] == NULL) {
            control->attempt_head[index] = node;
        } else {
            predecessors[index]->next[index] = node;
        }
    }
    for (uint8_t index = level; index < UTP_SEND_ATTEMPT_MAX_LEVEL; ++index) {
        node->next[index] = NULL;
    }
    ++control->attempt_count;
    return UTP_INTERNAL_ERROR_OK;
}

static uint64_t utp_send_control_packet_size(const utp_packet_out_t* packet)
{
    assert(packet != NULL);
    return (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
}

static void utp_send_control_packet_info(const utp_packet_out_t* packet, utp_congestion_packet_info_t* info)
{
    assert(packet != NULL);
    assert(info != NULL);
    info->packet_number = packet->packet_number;
    info->sent_time_us  = packet->sent_time_us;
    info->packet_size   = (uint32_t)utp_send_control_packet_size(packet);
    info->state         = packet->bw_state == NULL ? (void*)&packet->bw_packet_state : packet->bw_state;
}

static uint64_t utp_send_control_pacing_interval(const utp_send_control_t* control, uint64_t packet_size)
{
    uint64_t rate;
    uint64_t numerator;

    assert(control != NULL);
    assert(control->congestion != NULL);
    rate = utp_congestion_get_pacing_rate(control->congestion, 0);
    if (rate == 0u || packet_size == 0u || packet_size > UINT64_MAX / UINT64_C(1000000)) {
        return 1u;
    }
    numerator = packet_size * UINT64_C(1000000);
    return numerator / rate + (numerator % rate == 0u ? 0u : 1u);
}

static void utp_send_control_ack_result_add(utp_send_control_ack_result_t* result, const utp_packet_out_t* packet,
                                            uint64_t packet_number, uint64_t sent_time_us, bool in_flight)
{
    assert(result != NULL);
    assert(packet != NULL);
    const uint64_t packet_size = utp_send_control_packet_size(packet);

    if (in_flight) {
        result->ledger.acknowledged_bytes = packet_size > UINT64_MAX - result->ledger.acknowledged_bytes
                                                ? UINT64_MAX
                                                : result->ledger.acknowledged_bytes + packet_size;
    }
    ++result->ledger.acknowledged_packet_count;
    if (packet_number > result->ledger.largest_acknowledged_packet_number) {
        result->ledger.largest_acknowledged_packet_number = packet_number;
        result->ledger.largest_acknowledged_sent_time_us  = sent_time_us;
    }
}

static bool utp_send_control_ack_contains(const utp_ack_info_t* ack, uint64_t packet_number)
{
    assert(ack != NULL);
    for (size_t index = 0u; index < ack->range_count; ++index) {
        if (packet_number >= ack->ranges[index].low && packet_number <= ack->ranges[index].high) {
            return true;
        }
    }
    return false;
}

static utp_internal_error_t utp_send_control_acknowledge_packet(utp_send_control_t* control, const utp_ack_info_t* ack,
                                                                utp_packet_out_t* packet,
                                                                uint64_t          matched_packet_number,
                                                                uint64_t matched_sent_time_us, uint64_t now_us,
                                                                struct utp_packet_out_tailq*   acknowledged_packets,
                                                                utp_send_control_ack_result_t* result)
{
    assert(control != NULL);
    assert(ack != NULL);
    assert(packet != NULL);
    assert(acknowledged_packets != NULL);
    assert(result != NULL);
    assert(now_us != 0u);
    const bool           in_flight       = (packet->po_flags & UTP_PO_UNACKED) != 0u;
    const bool           current_attempt = in_flight && utp_send_control_ack_contains(ack, packet->packet_number);
    const uint64_t       acknowledged_packet_number = current_attempt ? packet->packet_number : matched_packet_number;
    const uint64_t       acknowledged_sent_time_us  = current_attempt ? packet->sent_time_us : matched_sent_time_us;
    utp_internal_error_t error;

    if (in_flight) {
        error = utp_send_ledger_remove(&control->ledger, packet);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    } else if ((packet->po_flags & UTP_PO_LOST) != 0u) {
        if (control->lost_packet_count == 0u) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        TAILQ_REMOVE(&control->lost_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_LOST;
        --control->lost_packet_count;
    } else {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (current_attempt && control->congestion != NULL) {
        utp_congestion_packet_info_t info;

        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_ack(control->congestion, &info, now_us, control->app_limited ? 1 : 0);
        packet->bw_state = info.state;
    }
    utp_send_control_ack_result_add(result, packet, acknowledged_packet_number, acknowledged_sent_time_us, in_flight);
    if (current_attempt && (!result->rtt_acknowledged_current_attempt ||
                            acknowledged_packet_number > result->rtt_acknowledged_packet_number)) {
        result->rtt_acknowledged_packet_number   = acknowledged_packet_number;
        result->rtt_acknowledged_sent_time_us    = acknowledged_sent_time_us;
        result->rtt_acknowledged_current_attempt = true;
    }
    utp_send_control_forget_packet_attempts(control, packet);
    TAILQ_INSERT_TAIL(acknowledged_packets, packet, po_next);
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_send_control_acknowledge_attempts(utp_send_control_t*            control,
                                                                  const utp_ack_info_t*          ack,
                                                                  struct utp_packet_out_tailq*   acknowledged_packets,
                                                                  utp_send_control_ack_result_t* result,
                                                                  uint64_t                       now_us)
{
    assert(control != NULL);
    assert(ack != NULL);
    assert(acknowledged_packets != NULL);
    assert(result != NULL);
    assert(now_us != 0u);
    for (size_t range_index = 0u; range_index < ack->range_count; ++range_index) {
        const utp_ack_range_t*   range = &ack->ranges[range_index];
        utp_send_attempt_node_t* node  = utp_send_control_attempt_lower_bound(control, range->low);

        while (node != NULL && node->packet_number <= range->high) {
            utp_packet_out_t*    packet        = node->packet;
            const uint64_t       packet_number = node->packet_number;
            utp_internal_error_t error         = utp_send_control_acknowledge_packet(
                control, ack, packet, packet_number, node->sent_time_us, now_us, acknowledged_packets, result);

            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            node = packet_number == UTP_PACKET_NUMBER_MAX
                       ? NULL
                       : utp_send_control_attempt_lower_bound(control, packet_number + 1u);
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_init(utp_send_control_t* control, size_t packet_limit,
                                           uint32_t retransmittable_frame_mask, uint64_t gap_warning_threshold,
                                           uint64_t peer_max_ack_delay_us)
{
    utp_internal_error_t error;

    if (control == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(control, 0, sizeof(*control));
    error = utp_send_ledger_init(&control->ledger, packet_limit, retransmittable_frame_mask);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    // 发送尝试索引按固定批次增长，不能跟随逻辑队列上限预分配。
    control->attempt_block_size = UTP_SEND_CONTROL_ATTEMPT_BLOCK_SIZE;
    control->attempt_level      = 1u;
    error                       = utp_send_control_attempt_add_block(control);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_send_ledger_cleanup(&control->ledger);
        return error;
    }
    utp_send_history_init(&control->send_history, gap_warning_threshold);
    TAILQ_INIT(&control->scheduled_packets);
    TAILQ_INIT(&control->lost_packets);
    TAILQ_INIT(&control->discarded_packets);
    control->peer_max_ack_delay_us  = peer_max_ack_delay_us;
    control->scheduled_packet_limit = packet_limit;
    control->reorder_threshold      = UTP_SEND_CONTROL_DEFAULT_REORDER_THRESHOLD;
    utp_pacer_init(&control->pacer, UTP_SEND_CONTROL_PACER_GRANULARITY_US);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_send_control_cleanup(utp_send_control_t* control)
{
    utp_packet_out_t* packet;

    if (control == NULL) {
        return;
    }
    while ((packet = TAILQ_FIRST(&control->scheduled_packets)) != NULL) {
        TAILQ_REMOVE(&control->scheduled_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_SCHED;
    }
    while ((packet = TAILQ_FIRST(&control->lost_packets)) != NULL) {
        TAILQ_REMOVE(&control->lost_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_LOST;
    }
    while ((packet = TAILQ_FIRST(&control->discarded_packets)) != NULL) {
        TAILQ_REMOVE(&control->discarded_packets, packet, po_next);
    }
    utp_send_ledger_cleanup(&control->ledger);
    while (control->attempt_blocks != NULL) {
        utp_send_attempt_block_t* block = control->attempt_blocks;

        control->attempt_blocks = block->next;
        utp_allocator_free(NULL, block->nodes);
        utp_allocator_free(NULL, block);
    }
    memset(control, 0, sizeof(*control));
}

bool utp_send_control_can_record_attempt(utp_send_control_t* control, const utp_packet_out_t* packet)
{
    assert(control != NULL);
    assert(packet != NULL);
    if ((packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) != 0u) {
        return true;
    }
    if (control->ledger.packet_count >= control->ledger.packet_limit) {
        return false;
    }
    return control->attempt_free != NULL || utp_send_control_attempt_add_block(control) == UTP_INTERNAL_ERROR_OK;
}

void utp_send_control_forget_packet_attempts(utp_send_control_t* control, utp_packet_out_t* packet)
{
    utp_send_attempt_node_t* node;

    assert(control != NULL);
    assert(packet != NULL);
    while ((node = packet->attempts) != NULL) {
        utp_send_control_attempt_release(control, node);
    }
}

utp_internal_error_t utp_send_control_on_packet_sent(utp_send_control_t* control, utp_packet_out_t* packet)
{
    utp_internal_error_t         error;
    utp_congestion_packet_info_t info;
    uint64_t                     packet_size;
    uint64_t                     inflight_before;
    uint64_t                     packet_count_before;

    assert(control != NULL);
    assert(packet != NULL);
    if (packet->sent_time_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if ((packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) != 0u) {
        // ACK-only 等非跟踪包仍占用包号；否则对端确认它们时会被误判为 ACK 了尚未发送的包。
        error = utp_send_history_update(&control->send_history, packet->packet_number);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        control->last_sent_time_us = packet->sent_time_us;
        if (packet->packet_number > control->current_packet_number) {
            control->current_packet_number = packet->packet_number;
        }
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_send_ledger_track(&control->ledger, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_control_record_attempt(control, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        (void)utp_send_ledger_remove(&control->ledger, packet);
        return error;
    }
    error = utp_send_history_update(&control->send_history, packet->packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        (void)utp_send_ledger_remove(&control->ledger, packet);
        utp_send_control_attempt_release(control, packet->attempts);
        return error;
    }
    packet_size         = utp_send_control_packet_size(packet);
    inflight_before     = utp_send_ledger_bytes_in_flight(&control->ledger) - packet_size;
    packet_count_before = utp_send_ledger_packet_count(&control->ledger) - 1u;
    if (control->congestion != NULL) {
        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_packet_sent(control->congestion, &info, inflight_before, control->app_limited ? 1 : 0);
        packet->bw_state = info.state;
    }
    if (control->pacing_enabled) {
        utp_pacer_packet_scheduled(&control->pacer, packet_count_before, false,
                                   utp_send_control_pacing_interval(control, packet_size));
    }
    control->last_sent_time_us = packet->sent_time_us;
    if (packet->packet_number > control->current_packet_number) {
        control->current_packet_number = packet->packet_number;
    }
    packet->po_flags    &= (uint16_t)~(UTP_PO_LOST | UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO);
    packet->local_flags &= (uint16_t)~(UTP_POL_FACKED | UTP_POL_LOSS);
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_allocate_packet_number(utp_send_control_t* control, uint64_t* packet_number)
{
    assert(control != NULL);
    assert(packet_number != NULL);
    if (control->current_packet_number >= UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    ++control->current_packet_number;
    *packet_number = control->current_packet_number;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_adopt_next_packet_number(utp_send_control_t* control, uint64_t next_packet_number)
{
    assert(control != NULL);
    if (next_packet_number == 0u || next_packet_number > UTP_PACKET_NUMBER_MAX + 1u ||
        next_packet_number <= control->current_packet_number) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    control->current_packet_number = next_packet_number - 1u;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_send_control_schedule_packet_at(utp_send_control_t* control, utp_packet_out_t* packet,
                                                                bool track_on_send, bool front)
{
    uint64_t packet_size;

    assert(control != NULL);
    assert(packet != NULL);
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (control->scheduled_packet_count >= control->scheduled_packet_limit) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    packet_size = utp_send_control_packet_size(packet);
    if (packet_size > UINT64_MAX - control->scheduled_byte_count) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }

    if (front) {
        TAILQ_INSERT_HEAD(&control->scheduled_packets, packet, po_next);
    } else {
        TAILQ_INSERT_TAIL(&control->scheduled_packets, packet, po_next);
    }
    packet->po_flags |= UTP_PO_SCHED;
    if (track_on_send) {
        packet->local_flags |= UTP_POL_TRACK_ON_SEND;
        packet->local_flags &= (uint16_t)~UTP_POL_NO_TRACK_ON_SEND;
    } else {
        packet->local_flags &= (uint16_t)~UTP_POL_TRACK_ON_SEND;
        packet->local_flags |= UTP_POL_NO_TRACK_ON_SEND;
    }
    control->scheduled_byte_count += packet_size;
    ++control->scheduled_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_schedule_packet(utp_send_control_t* control, utp_packet_out_t* packet,
                                                      bool track_on_send)
{
    return utp_send_control_schedule_packet_at(control, packet, track_on_send, false);
}

utp_internal_error_t utp_send_control_schedule_packet_front(utp_send_control_t* control, utp_packet_out_t* packet,
                                                            bool track_on_send)
{
    return utp_send_control_schedule_packet_at(control, packet, track_on_send, true);
}

utp_internal_error_t utp_send_control_reschedule_packet(utp_send_control_t* control, utp_packet_out_t* packet)
{
    uint64_t packet_size;

    assert(control != NULL);
    assert(packet != NULL);
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (control->scheduled_packet_count >= control->scheduled_packet_limit) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    packet_size = utp_send_control_packet_size(packet);
    if (packet_size > UINT64_MAX - control->scheduled_byte_count) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    TAILQ_INSERT_HEAD(&control->scheduled_packets, packet, po_next);
    packet->po_flags              |= UTP_PO_SCHED;
    control->scheduled_byte_count += packet_size;
    ++control->scheduled_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t* utp_send_control_next_scheduled(utp_send_control_t* control)
{
    utp_packet_out_t* packet;
    uint64_t          packet_size;

    assert(control != NULL);
    if ((packet = TAILQ_FIRST(&control->scheduled_packets)) == NULL) {
        return NULL;
    }
    packet_size = utp_send_control_packet_size(packet);
    assert(control->scheduled_packet_count != 0u);
    assert(packet_size <= control->scheduled_byte_count);

    TAILQ_REMOVE(&control->scheduled_packets, packet, po_next);
    packet->po_flags &= (uint16_t)~UTP_PO_SCHED;
    --control->scheduled_packet_count;
    control->scheduled_byte_count -= packet_size;
    return packet;
}

utp_packet_out_t* utp_send_control_peek_scheduled(const utp_send_control_t* control)
{
    assert(control != NULL);
    return TAILQ_FIRST(&control->scheduled_packets);
}

static bool utp_send_control_packet_is_retransmittable(const utp_send_control_t* control,
                                                       const utp_packet_out_t*   packet)
{
    assert(control != NULL);
    assert(packet != NULL);
    return (packet->frame_types & control->ledger.retransmittable_frame_mask) != 0u;
}

static bool utp_send_control_is_fack_lost(const utp_send_control_t* control, const utp_packet_out_t* packet)
{
    assert(control != NULL);
    assert(packet != NULL);
    return control->largest_acked_packet_number > control->reorder_threshold &&
           packet->packet_number < control->largest_acked_packet_number - control->reorder_threshold;
}

static bool utp_send_control_is_time_lost(const utp_send_control_t* control, const utp_packet_out_t* packet)
{
    uint64_t srtt;

    assert(control != NULL);
    assert(packet != NULL);
    srtt = utp_rtt_stats_srtt(&control->rtt_stats);
    return srtt != 0u && control->largest_acked_sent_time_us > srtt &&
           packet->sent_time_us < control->largest_acked_sent_time_us - srtt;
}

static utp_internal_error_t utp_send_control_mark_packet_lost(utp_send_control_t* control, utp_packet_out_t* packet,
                                                              bool fack_lost)
{
    utp_internal_error_t         error;
    utp_congestion_packet_info_t info;

    assert(control != NULL);
    assert(packet != NULL);
    assert((packet->po_flags & UTP_PO_UNACKED) != 0u);
    if (control->congestion != NULL && (packet->po_flags & UTP_PO_MTU_PROBE) == 0u) {
        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_lost(control->congestion, &info);
        packet->bw_state = info.state;
    }

    error = utp_send_ledger_remove(&control->ledger, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if ((packet->po_flags & UTP_PO_MTU_PROBE) != 0u || !utp_send_control_packet_is_retransmittable(control, packet)) {
        TAILQ_INSERT_TAIL(&control->discarded_packets, packet, po_next);
        ++control->discarded_packet_count;
        return UTP_INTERNAL_ERROR_OK;
    }
    packet->po_flags |= UTP_PO_LOST | UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO;
    if (fack_lost) {
        packet->local_flags |= UTP_POL_FACKED;
    }
    packet->loss_chain = packet;
    TAILQ_INSERT_TAIL(&control->lost_packets, packet, po_next);
    ++control->lost_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_detect_losses(utp_send_control_t* control)
{
    utp_packet_out_t*    packet;
    utp_packet_out_t*    next;
    utp_internal_error_t error;
    uint64_t             largest_lost_packet_number = 0u;

    if (control == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (packet = TAILQ_FIRST(&control->ledger.unacked_packets); packet != NULL; packet = next) {
        bool fack_lost;
        bool time_lost;

        next = TAILQ_NEXT(packet, po_next);
        if (packet->packet_number > control->largest_acked_packet_number ||
            (packet->po_flags & UTP_PO_LOSS_RECORDED) != 0u) {
            continue;
        }
        fack_lost = utp_send_control_is_fack_lost(control, packet);
        time_lost = utp_send_control_is_time_lost(control, packet);
        if (!fack_lost && !time_lost) {
            continue;
        }

        if (utp_send_control_packet_is_retransmittable(control, packet) &&
            (packet->po_flags & UTP_PO_MTU_PROBE) == 0u && packet->packet_number > largest_lost_packet_number) {
            largest_lost_packet_number = packet->packet_number;
        }

        error = utp_send_control_mark_packet_lost(control, packet, fack_lost);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (largest_lost_packet_number > control->largest_sent_at_cutback) {
        utp_congestion_on_loss(control->congestion);
        if (control->pacing_enabled) {
            utp_pacer_on_loss(&control->pacer);
        }
        control->largest_sent_at_cutback = utp_send_history_largest(&control->send_history);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t* utp_send_control_next_lost(utp_send_control_t* control)
{
    utp_packet_out_t* packet;

    assert(control != NULL);
    if ((packet = TAILQ_FIRST(&control->lost_packets)) == NULL) {
        return NULL;
    }
    assert(control->lost_packet_count != 0u);
    TAILQ_REMOVE(&control->lost_packets, packet, po_next);
    packet->po_flags &= (uint16_t)~UTP_PO_LOST;
    --control->lost_packet_count;
    return packet;
}

utp_internal_error_t utp_send_control_reschedule_lost(utp_send_control_t* control, utp_packet_out_t* packet)
{
    assert(control != NULL);
    assert(packet != NULL);
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    TAILQ_INSERT_HEAD(&control->lost_packets, packet, po_next);
    packet->po_flags |= UTP_PO_LOST;
    ++control->lost_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t* utp_send_control_next_discarded(utp_send_control_t* control)
{
    utp_packet_out_t* packet;

    assert(control != NULL);
    if ((packet = TAILQ_FIRST(&control->discarded_packets)) == NULL) {
        return NULL;
    }
    assert(control->discarded_packet_count != 0u);
    TAILQ_REMOVE(&control->discarded_packets, packet, po_next);
    --control->discarded_packet_count;
    return packet;
}

utp_internal_error_t utp_send_control_take_mtu_probe(utp_send_control_t* control, uint64_t packet_number,
                                                     utp_packet_out_t** out_packet)
{
    utp_packet_out_t* packet;

    if (control == NULL || out_packet == NULL || packet_number == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *out_packet = NULL;
    TAILQ_FOREACH(packet, &control->ledger.unacked_packets, po_next)
    {
        if (packet->packet_number != packet_number || (packet->po_flags & UTP_PO_MTU_PROBE) == 0u) {
            continue;
        }
        if (utp_send_ledger_remove(&control->ledger, packet) != UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        *out_packet = packet;
        return UTP_INTERNAL_ERROR_OK;
    }
    return UTP_INTERNAL_ERROR_NOT_FOUND;
}

void utp_send_control_set_connected(utp_send_control_t* control, bool connected)
{
    assert(control != NULL);
    control->connected = connected;
}

void utp_send_control_set_loss_pending(utp_send_control_t* control, bool pending)
{
    assert(control != NULL);
    control->loss_pending = pending;
}

void utp_send_control_set_congestion(utp_send_control_t* control, utp_congestion_t* congestion)
{
    assert(control != NULL);
    assert(congestion != NULL);
    control->congestion = congestion;
    utp_congestion_init(congestion, &control->rtt_stats);
}

void utp_send_control_set_pacing_enabled(utp_send_control_t* control, bool enabled, uint32_t clock_granularity_us)
{
    assert(control != NULL);
    control->pacing_enabled = enabled;
    utp_pacer_init(&control->pacer,
                   clock_granularity_us == 0u ? UTP_SEND_CONTROL_PACER_GRANULARITY_US : clock_granularity_us);
}

void utp_send_control_set_app_limited(utp_send_control_t* control, bool app_limited)
{
    assert(control != NULL);
    control->app_limited = app_limited;
}

void utp_send_control_pacer_tick_in(utp_send_control_t* control, uint64_t now_us)
{
    assert(control != NULL);
    if (control->pacing_enabled) {
        utp_pacer_tick_in(&control->pacer, now_us);
    }
}

void utp_send_control_pacer_tick_out(utp_send_control_t* control)
{
    assert(control != NULL);
    if (control->pacing_enabled) {
        utp_pacer_tick_out(&control->pacer);
    }
}

bool utp_send_control_can_schedule_packet(const utp_send_control_t* control, uint64_t packet_size)
{
    uint64_t used;
    uint64_t cwnd;

    assert(control != NULL);
    assert(control->congestion != NULL);
    assert(packet_size != 0u);
    used = utp_send_ledger_bytes_in_flight(&control->ledger);
    if (used > UINT64_MAX - control->scheduled_byte_count) {
        return false;
    }
    used += control->scheduled_byte_count;
    if (used == 0u) {
        return true;
    }
    cwnd = utp_congestion_get_cwnd(control->congestion);
    return used < cwnd && packet_size <= cwnd - used;
}

bool utp_send_control_can_transmit_packet(utp_send_control_t* control, uint64_t packet_size)
{
    uint64_t inflight;
    uint64_t cwnd;

    assert(control != NULL);
    assert(control->congestion != NULL);
    assert(packet_size != 0u);
    inflight = utp_send_ledger_bytes_in_flight(&control->ledger);
    cwnd     = utp_congestion_get_cwnd(control->congestion);
    if (inflight != 0u && (inflight >= cwnd || packet_size > cwnd - inflight)) {
        return false;
    }
    return !control->pacing_enabled ||
           utp_pacer_can_schedule(&control->pacer, utp_send_ledger_packet_count(&control->ledger));
}

uint64_t utp_send_control_pacing_deadline(const utp_send_control_t* control)
{
    assert(control != NULL);
    if (!control->pacing_enabled || !utp_pacer_delayed(&control->pacer)) {
        return 0u;
    }
    return utp_pacer_next_scheduled_time(&control->pacer);
}

static bool utp_send_control_has_unacked_handshake_packet(const utp_send_control_t* control)
{
    utp_packet_out_t* packet;

    assert(control != NULL);
    if (control->connected) {
        return false;
    }
    TAILQ_FOREACH(packet, &control->ledger.unacked_packets, po_next)
    {
        if ((packet->po_flags & UTP_PO_HELLO) != 0u) {
            return true;
        }
    }
    return false;
}

utp_send_control_retransmission_mode_t utp_send_control_retransmission_mode(const utp_send_control_t* control)
{
    assert(control != NULL);
    if (TAILQ_EMPTY(&control->ledger.unacked_packets)) {
        return UTP_SEND_CONTROL_RETRANSMISSION_RTO;
    }
    if (utp_send_control_has_unacked_handshake_packet(control)) {
        return UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE;
    }
    if (control->loss_pending) {
        return UTP_SEND_CONTROL_RETRANSMISSION_LOSS;
    }
    if (control->tlp_count < UTP_SEND_CONTROL_MAX_TLP_COUNT) {
        return UTP_SEND_CONTROL_RETRANSMISSION_TLP;
    }
    return UTP_SEND_CONTROL_RETRANSMISSION_RTO;
}

uint64_t utp_send_control_calculate_handshake_delay(utp_send_control_t* control)
{
    uint64_t delay;
    uint32_t exponent;

    assert(control != NULL);
    delay = utp_rtt_stats_srtt(&control->rtt_stats);
    if (delay == 0u) {
        delay = 150000u;
    } else {
        delay += delay / 2u;
        if (delay < 10000u) {
            delay = 10000u;
        }
    }
    exponent = control->handshake_retransmission_count > 8u ? 8u : control->handshake_retransmission_count;
    if (control->handshake_retransmission_count != UINT32_MAX) {
        ++control->handshake_retransmission_count;
    }
    while (exponent-- != 0u) {
        if (delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US / 2u) {
            return UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US;
        }
        delay *= 2u;
    }
    return delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US : delay;
}

uint64_t utp_send_control_calculate_tlp_delay(const utp_send_control_t* control)
{
    uint64_t srtt;
    uint64_t delay;

    assert(control != NULL);
    srtt = utp_rtt_stats_srtt(&control->rtt_stats);
    if (srtt == 0u) {
        srtt = UTP_SEND_CONTROL_INITIAL_RTT_US;
    }
    if (utp_send_ledger_packet_count(&control->ledger) > 1u) {
        delay = 10000u;
    } else if (srtt > UINT64_MAX - srtt / 2u) {
        delay = UINT64_MAX;
    } else {
        delay = srtt + srtt / 2u;
        if (delay > UINT64_MAX - control->peer_max_ack_delay_us) {
            delay = UINT64_MAX;
        } else {
            delay += control->peer_max_ack_delay_us;
        }
    }
    if (srtt <= UINT64_MAX / 2u && delay < srtt * 2u) {
        delay = srtt * 2u;
    }
    return delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US : delay;
}

uint64_t utp_send_control_calculate_rto(const utp_send_control_t* control)
{
    uint64_t base_delay;
    uint64_t srtt;
    uint64_t variance;
    uint64_t factor;
    uint32_t exponent;

    assert(control != NULL);
    srtt     = utp_rtt_stats_srtt(&control->rtt_stats);
    variance = utp_rtt_stats_variance(&control->rtt_stats);
    if (srtt == 0u) {
        base_delay = UTP_SEND_CONTROL_DEFAULT_RTO_US;
    } else if (variance > (UINT64_MAX - srtt) / 4u) {
        base_delay = UINT64_MAX;
    } else {
        base_delay = srtt + 4u * variance;
        if (base_delay < UTP_SEND_CONTROL_MIN_RTO_US) {
            base_delay = UTP_SEND_CONTROL_MIN_RTO_US;
        }
    }
    exponent = control->consecutive_rto_count > UTP_SEND_CONTROL_MAX_RTO_BACKOFFS ? UTP_SEND_CONTROL_MAX_RTO_BACKOFFS
                                                                                  : control->consecutive_rto_count;
    factor   = UINT64_C(1) << exponent;
    if (base_delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US / factor) {
        return UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US;
    }
    base_delay *= factor;
    return base_delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US
                                                                     : base_delay;
}

typedef enum utp_send_control_expire_filter {
    UTP_SEND_CONTROL_EXPIRE_ALL,
    UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY,
    UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE
} utp_send_control_expire_filter_t;

static utp_internal_error_t utp_send_control_expire_unacked(utp_send_control_t*              control,
                                                            utp_send_control_expire_filter_t filter)
{
    utp_packet_out_t*    packet;
    utp_packet_out_t*    next;
    utp_internal_error_t error;

    assert(control != NULL);
    if (filter == UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE) {
        TAILQ_FOREACH_REVERSE(packet, &control->ledger.unacked_packets, utp_packet_out_tailq, po_next)
        {
            if (utp_send_control_packet_is_retransmittable(control, packet) &&
                (packet->po_flags & UTP_PO_LOSS_RECORDED) == 0u) {
                return utp_send_control_mark_packet_lost(control, packet, false);
            }
        }
        return UTP_INTERNAL_ERROR_OK;
    }

    for (packet = TAILQ_FIRST(&control->ledger.unacked_packets); packet != NULL; packet = next) {
        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_LOSS_RECORDED) != 0u ||
            (filter == UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY && (packet->po_flags & UTP_PO_HELLO) == 0u) ||
            ((packet->po_flags & UTP_PO_MTU_PROBE) == 0u &&
             !utp_send_control_packet_is_retransmittable(control, packet))) {
            continue;
        }
        error = utp_send_control_mark_packet_lost(control, packet, false);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_on_retransmission_timeout(utp_send_control_t* control)
{
    assert(control != NULL);
    if (TAILQ_EMPTY(&control->ledger.unacked_packets)) {
        control->loss_pending = false;
        return UTP_INTERNAL_ERROR_OK;
    }

    switch (utp_send_control_retransmission_mode(control)) {
    case UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE:
        return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY);
    case UTP_SEND_CONTROL_RETRANSMISSION_LOSS:
        control->loss_pending = false;
        return utp_send_control_detect_losses(control);
    case UTP_SEND_CONTROL_RETRANSMISSION_TLP:
        if (control->tlp_count != UINT32_MAX) {
            ++control->tlp_count;
        }
        return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE);
    case UTP_SEND_CONTROL_RETRANSMISSION_RTO:
        if (control->consecutive_rto_count != UINT32_MAX) {
            ++control->consecutive_rto_count;
        }
        utp_congestion_on_timeout(control->congestion);
        return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_ALL);
    }
    return UTP_INTERNAL_ERROR_STATE;
}

static utp_internal_error_t utp_send_control_on_ack_internal(utp_send_control_t* control, const utp_ack_info_t* ack,
                                                             uint64_t now_us, bool exact_handshake_delay,
                                                             uint64_t                       handshake_delay_us,
                                                             struct utp_packet_out_tailq*   acknowledged_packets,
                                                             utp_send_control_ack_result_t* result)
{
    utp_internal_error_t error;

    assert(control != NULL);
    assert(ack != NULL);
    assert(acknowledged_packets != NULL);
    assert(result != NULL);
    assert(now_us != 0u);
    result->ledger.largest_acknowledged_packet_number = 0u;
    result->ledger.largest_acknowledged_sent_time_us  = 0u;
    result->ledger.acknowledged_bytes                 = 0u;
    result->ledger.acknowledged_packet_count          = 0u;
    result->rtt_acknowledged_packet_number            = 0u;
    result->rtt_acknowledged_sent_time_us             = 0u;
    result->rtt_sample_us                             = 0u;
    result->rtt_acknowledged_current_attempt          = false;
    result->rtt_sample_valid                          = false;
    {
        const uint64_t inflight_before_ack = utp_send_ledger_bytes_in_flight(&control->ledger);

        error = utp_send_ledger_validate_ack(ack, utp_send_history_largest(&control->send_history));
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (control->was_quiet) {
            control->was_quiet = false;
            utp_congestion_was_quiet(control->congestion, now_us, inflight_before_ack);
        }
        utp_congestion_on_begin_ack(control->congestion, now_us, inflight_before_ack);
        error = utp_send_control_acknowledge_attempts(control, ack, acknowledged_packets, result, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_congestion_on_end_ack(control->congestion, utp_send_ledger_bytes_in_flight(&control->ledger));
            return error;
        }
    }
    utp_congestion_on_end_ack(control->congestion, utp_send_ledger_bytes_in_flight(&control->ledger));
    if (ack->largest_acked > control->largest_acked_packet_number) {
        control->largest_acked_packet_number = ack->largest_acked;
    }
    if (result->rtt_acknowledged_current_attempt) {
        uint64_t acknowledged_sent_time_us = result->rtt_acknowledged_sent_time_us;

        control->largest_acked_sent_time_us = acknowledged_sent_time_us;
        if (now_us > acknowledged_sent_time_us) {
            if (exact_handshake_delay) {
                const uint64_t elapsed_us = now_us - acknowledged_sent_time_us;

                error = elapsed_us <= handshake_delay_us
                            ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT
                            : utp_rtt_stats_update(&control->rtt_stats, elapsed_us - handshake_delay_us);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    result->rtt_sample_us = elapsed_us - handshake_delay_us;
                }
            } else {
                error = utp_rtt_stats_update_from_ack(&control->rtt_stats, now_us, acknowledged_sent_time_us,
                                                      ack->ack_delay, control->peer_max_ack_delay_us,
                                                      &result->rtt_sample_us);
            }
            result->rtt_sample_valid = error == UTP_INTERNAL_ERROR_OK;
            if (error == UTP_INTERNAL_ERROR_INVALID_ARGUMENT) {
                error = UTP_INTERNAL_ERROR_OK;
            }
        }
    }
    if (result->ledger.acknowledged_packet_count != 0u) {
        control->consecutive_rto_count          = 0u;
        control->handshake_retransmission_count = 0u;
        control->tlp_count                      = 0u;
        control->loss_pending                   = false;
    }
    if (utp_send_ledger_retransmittable_packet_count(&control->ledger) == 0u) {
        control->was_quiet = true;
    }
    return utp_send_control_detect_losses(control);
}

utp_internal_error_t utp_send_control_on_ack(utp_send_control_t* control, const utp_ack_info_t* ack, uint64_t now_us,
                                             struct utp_packet_out_tailq*   acknowledged_packets,
                                             utp_send_control_ack_result_t* result)
{
    return utp_send_control_on_ack_internal(control, ack, now_us, false, 0u, acknowledged_packets, result);
}

utp_internal_error_t utp_send_control_on_handshake_ack(utp_send_control_t* control, const utp_ack_info_t* ack,
                                                       uint64_t now_us, uint64_t handshake_delay_us,
                                                       struct utp_packet_out_tailq*   acknowledged_packets,
                                                       utp_send_control_ack_result_t* result)
{
    return utp_send_control_on_ack_internal(control, ack, now_us, true, handshake_delay_us, acknowledged_packets,
                                            result);
}

utp_internal_error_t utp_send_control_retire_handshake_packets(utp_send_control_t* control, uint64_t now_us,
                                                               struct utp_packet_out_tailq* retired_packets)
{
    utp_packet_out_t* packet;
    utp_packet_out_t* next;

    assert(control != NULL);
    assert(retired_packets != NULL);
    assert(now_us != 0u);
    for (packet = TAILQ_FIRST(&control->scheduled_packets); packet != NULL; packet = next) {
        uint64_t packet_size;

        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_HELLO) == 0u) {
            continue;
        }
        packet_size = utp_send_control_packet_size(packet);
        if (control->scheduled_packet_count == 0u || packet_size > control->scheduled_byte_count) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        TAILQ_REMOVE(&control->scheduled_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_SCHED;
        --control->scheduled_packet_count;
        control->scheduled_byte_count -= packet_size;
        TAILQ_INSERT_TAIL(retired_packets, packet, po_next);
    }
    for (packet = TAILQ_FIRST(&control->ledger.unacked_packets); packet != NULL; packet = next) {
        utp_internal_error_t error;

        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_HELLO) == 0u) {
            continue;
        }
        error = utp_send_ledger_remove(&control->ledger, packet);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        TAILQ_INSERT_TAIL(retired_packets, packet, po_next);
    }
    for (packet = TAILQ_FIRST(&control->lost_packets); packet != NULL; packet = next) {
        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_HELLO) == 0u) {
            continue;
        }
        if (control->lost_packet_count == 0u) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        TAILQ_REMOVE(&control->lost_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_LOST;
        --control->lost_packet_count;
        TAILQ_INSERT_TAIL(retired_packets, packet, po_next);
    }
    for (packet = TAILQ_FIRST(&control->discarded_packets); packet != NULL; packet = next) {
        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_HELLO) == 0u) {
            continue;
        }
        if (control->discarded_packet_count == 0u) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        TAILQ_REMOVE(&control->discarded_packets, packet, po_next);
        --control->discarded_packet_count;
        TAILQ_INSERT_TAIL(retired_packets, packet, po_next);
    }
    if (utp_send_ledger_retransmittable_packet_count(&control->ledger) == 0u) {
        control->was_quiet = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_send_control_largest_sent(const utp_send_control_t* control)
{
    assert(control != NULL);
    return utp_send_history_largest(&control->send_history);
}

uint64_t utp_send_control_largest_acked(const utp_send_control_t* control)
{
    assert(control != NULL);
    return control->largest_acked_packet_number;
}

size_t utp_send_control_unacked_packet_count(const utp_send_control_t* control)
{
    assert(control != NULL);
    return utp_send_ledger_packet_count(&control->ledger);
}

size_t utp_send_control_scheduled_packet_count(const utp_send_control_t* control)
{
    assert(control != NULL);
    return control->scheduled_packet_count;
}

uint64_t utp_send_control_scheduled_bytes(const utp_send_control_t* control)
{
    assert(control != NULL);
    return control->scheduled_byte_count;
}

size_t utp_send_control_lost_packet_count(const utp_send_control_t* control)
{
    assert(control != NULL);
    return control->lost_packet_count;
}

size_t utp_send_control_discarded_packet_count(const utp_send_control_t* control)
{
    assert(control != NULL);
    return control->discarded_packet_count;
}

uint64_t utp_send_control_srtt(const utp_send_control_t* control)
{
    assert(control != NULL);
    return utp_rtt_stats_srtt(&control->rtt_stats);
}

uint64_t utp_send_control_bandwidth_estimate(const utp_send_control_t* control)
{
    assert(control != NULL);
    assert(control->congestion != NULL);
    return utp_congestion_get_pacing_rate(control->congestion, 0);
}
