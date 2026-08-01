#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/congestion.h"
#include "context/send_control.h"
}

namespace {

utp_packet_out_t make_packet(uint64_t packet_number, uint64_t sent_time_us)
{
    utp_packet_out_t packet = {};

    packet.packet_number = packet_number;
    packet.sent_time_us  = sent_time_us;
    packet.data_size     = 100u;
    return packet;
}

struct CongestionTrace {
    uint64_t cwnd           = 0u;
    uint64_t sent_inflight  = 0u;
    uint64_t begin_inflight = 0u;
    uint32_t sent_calls     = 0u;
    uint32_t ack_calls      = 0u;
    uint32_t begin_calls    = 0u;
    uint32_t end_calls      = 0u;
};

uint64_t trace_cwnd(void* state) { return static_cast<CongestionTrace*>(state)->cwnd; }

uint64_t trace_rate(void*, int32_t) { return 1000000u; }

void trace_sent(void* state, utp_congestion_packet_info_t* packet, uint64_t inflight_bytes, int32_t)
{
    auto* trace = static_cast<CongestionTrace*>(state);

    REQUIRE(packet != nullptr);
    trace->sent_inflight = inflight_bytes;
    ++trace->sent_calls;
}

void trace_begin(void* state, uint64_t, uint64_t inflight_bytes)
{
    auto* trace = static_cast<CongestionTrace*>(state);

    trace->begin_inflight = inflight_bytes;
    ++trace->begin_calls;
}

void trace_ack(void* state, utp_congestion_packet_info_t* packet, uint64_t, int32_t)
{
    REQUIRE(packet != nullptr);
    ++static_cast<CongestionTrace*>(state)->ack_calls;
}

void trace_end(void* state, uint64_t) { ++static_cast<CongestionTrace*>(state)->end_calls; }

const utp_congestion_ops_t kTraceCongestionOps = {
    nullptr, trace_cwnd, trace_rate, trace_begin, trace_sent, trace_ack, nullptr, nullptr, trace_end, nullptr, nullptr,
};

}  // namespace

TEST_CASE("send control applies congestion admission and transaction callbacks", "[send_control][congestion]")
{
    utp_send_control_t            control    = {};
    utp_packet_out_t              packet     = make_packet(1u, 100u);
    CongestionTrace               trace      = {};
    utp_congestion_t              congestion = {&trace, &kTraceCongestionOps};
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{1u, 1u}};
    const utp_ack_info_t          ack      = {1u, 0u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    trace.cwnd = 100u;
    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    utp_send_control_set_congestion(&control, &congestion);
    REQUIRE(utp_send_control_can_schedule_packet(&control, 100u));
    REQUIRE(utp_send_control_can_transmit_packet(&control, 100u));

    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(trace.sent_calls == 1u);
    REQUIRE(trace.sent_inflight == 0u);
    REQUIRE_FALSE(utp_send_control_can_transmit_packet(&control, 1u));

    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(trace.begin_calls == 1u);
    REQUIRE(trace.begin_inflight == 100u);
    REQUIRE(trace.ack_calls == 1u);
    REQUIRE(trace.end_calls == 1u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control records sent packets and applies an ACK", "[send_control]")
{
    utp_send_control_t            control = {};
    utp_packet_out_t              first   = make_packet(1u, 100u);
    utp_packet_out_t              second  = make_packet(2u, 200u);
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{2u, 2u}};
    const utp_ack_info_t          ack      = {2u, 50u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 4u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &second) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(first.attempt_count == 1u);
    REQUIRE(first.attempts[0].packet_number == 1u);
    REQUIRE(utp_send_control_largest_sent(&control) == 2u);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 2u);

    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(result.ledger.acknowledged_packet_count == 1u);
    REQUIRE(result.ledger.largest_acknowledged_packet_number == 2u);
    REQUIRE(result.rtt_sample_valid);
    REQUIRE(result.rtt_sample_us == 750u);
    REQUIRE(TAILQ_FIRST(&acknowledged) == &second);
    REQUIRE(utp_send_control_largest_acked(&control) == 2u);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 1u);
    REQUIRE(utp_send_control_srtt(&control) == 750u);

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control rejects an invalid ACK without changing pending packets", "[send_control]")
{
    utp_send_control_t            control = {};
    utp_packet_out_t              packet  = make_packet(1u, 100u);
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{2u, 2u}};
    const utp_ack_info_t          ack      = {2u, 0u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 1u);
    REQUIRE(utp_send_control_largest_acked(&control) == 0u);
    REQUIRE(TAILQ_EMPTY(&acknowledged));

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control rejects a sent packet without a timestamp", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   packet  = make_packet(1u, 0u);

    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_send_control_largest_sent(&control) == 0u);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 0u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control schedules packets in FIFO order without ACK tracking", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   first   = make_packet(1u, 100u);
    utp_packet_out_t   second  = make_packet(2u, 200u);

    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &first, true) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &second, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_scheduled_packet_count(&control) == 2u);
    REQUIRE(utp_send_control_scheduled_bytes(&control) == 200u);
    REQUIRE((first.po_flags & UTP_PO_SCHED) != 0u);
    REQUIRE((first.local_flags & UTP_POL_TRACK_ON_SEND) != 0u);

    REQUIRE(utp_send_control_next_scheduled(&control) == &first);
    REQUIRE((first.po_flags & UTP_PO_SCHED) == 0u);
    REQUIRE(utp_send_control_scheduled_packet_count(&control) == 1u);
    REQUIRE(utp_send_control_next_scheduled(&control) == &second);
    REQUIRE(utp_send_control_next_scheduled(&control) == nullptr);
    REQUIRE(utp_send_control_scheduled_bytes(&control) == 0u);

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control does not mutate a packet when its scheduling queue is full", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   first   = make_packet(1u, 100u);
    utp_packet_out_t   second  = make_packet(2u, 200u);

    REQUIRE(utp_send_control_init(&control, 1u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &first, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &second, true) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE((second.po_flags & UTP_PO_SCHED) == 0u);
    REQUIRE((second.local_flags & UTP_POL_TRACK_ON_SEND) == 0u);

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control rejects a packet that is still owned by a send queue", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   packet  = make_packet(1u, 100u);

    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &packet, true) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_STATE);
    REQUIRE(utp_send_control_scheduled_packet_count(&control) == 1u);
    REQUIRE((packet.po_flags & UTP_PO_SCHED) != 0u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control does not track a sent packet without TRACK_ON_SEND", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   packet  = make_packet(1u, 100u);

    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_schedule_packet(&control, &packet, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_next_scheduled(&control) == &packet);
    packet.sent_time_us = 50u;
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 0u);
    REQUIRE(packet.attempt_count == 1u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control moves FACK-lost retransmittable packets to the loss queue", "[send_control]")
{
    utp_send_control_t            control    = {};
    utp_packet_out_t              packets[5] = {};
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{5u, 5u}};
    const utp_ack_info_t          ack      = {5u, 0u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 8u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    for (uint64_t number = 1u; number <= 5u; ++number) {
        packets[number - 1u]             = make_packet(number, number * 100u);
        packets[number - 1u].frame_types = UINT32_C(0x01);
        REQUIRE(utp_send_control_on_packet_sent(&control, &packets[number - 1u]) == UTP_INTERNAL_ERROR_OK);
    }

    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_lost_packet_count(&control) == 1u);
    REQUIRE(utp_send_control_next_lost(&control) == &packets[0]);
    REQUIRE((packets[0].po_flags & (UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO)) ==
            (UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO));
    REQUIRE((packets[0].po_flags & UTP_PO_LOST) == 0u);
    packets[0].packet_number = 6u;
    packets[0].sent_time_us  = 2000u;
    REQUIRE(utp_send_control_on_packet_sent(&control, &packets[0]) == UTP_INTERNAL_ERROR_OK);
    REQUIRE((packets[0].po_flags & (UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO)) == 0u);
    REQUIRE((packets[0].local_flags & UTP_POL_FACKED) == 0u);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 4u);

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control returns lost MTU probes and non-retransmittable packets for release", "[send_control]")
{
    utp_send_control_t            control    = {};
    utp_packet_out_t              packets[6] = {};
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{6u, 6u}};
    const utp_ack_info_t          ack      = {6u, 0u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 8u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    for (uint64_t number = 1u; number <= 6u; ++number) {
        packets[number - 1u] = make_packet(number, number * 100u);
        if (number == 1u) {
            packets[number - 1u].po_flags = UTP_PO_MTU_PROBE;
        }
        if (number != 2u) {
            packets[number - 1u].frame_types = UINT32_C(0x01);
        }
        REQUIRE(utp_send_control_on_packet_sent(&control, &packets[number - 1u]) == UTP_INTERNAL_ERROR_OK);
    }

    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_lost_packet_count(&control) == 0u);
    REQUIRE(utp_send_control_discarded_packet_count(&control) == 2u);
    REQUIRE(utp_send_control_next_discarded(&control) == &packets[0]);
    REQUIRE(utp_send_control_next_discarded(&control) == &packets[1]);
    REQUIRE((packets[0].po_flags & (UTP_PO_UNACKED | UTP_PO_LOST)) == 0u);

    utp_send_control_cleanup(&control);
}

TEST_CASE("send control detects send-time loss once without a FACK signal", "[send_control]")
{
    utp_send_control_t            control    = {};
    utp_packet_out_t              packets[4] = {};
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               first_ranges[]  = {{2u, 2u}};
    utp_ack_range_t               second_ranges[] = {{4u, 4u}};
    const utp_ack_info_t          first_ack       = {2u, 0u, first_ranges, 1u, 1u};
    const utp_ack_info_t          second_ack      = {4u, 0u, second_ranges, 1u, 1u};
    utp_send_control_ack_result_t result          = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 8u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    for (uint64_t number = 1u; number <= 2u; ++number) {
        packets[number - 1u]             = make_packet(number, number * 100u);
        packets[number - 1u].frame_types = UINT32_C(0x01);
        REQUIRE(utp_send_control_on_packet_sent(&control, &packets[number - 1u]) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_send_control_on_ack(&control, &first_ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);

    packets[2]             = make_packet(3u, 300u);
    packets[2].frame_types = UINT32_C(0x01);
    packets[3]             = make_packet(4u, 2000u);
    packets[3].frame_types = UINT32_C(0x01);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packets[2]) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packets[3]) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_ack(&control, &second_ack, 3000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_lost_packet_count(&control) == 2u);
    REQUIRE((packets[0].local_flags & UTP_POL_FACKED) == 0u);
    REQUIRE((packets[2].local_flags & UTP_POL_FACKED) == 0u);
    REQUIRE(utp_send_control_detect_losses(&control) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_lost_packet_count(&control) == 2u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control applies bounded handshake, TLP, and RTO delays", "[send_control]")
{
    utp_send_control_t control = {};

    REQUIRE(utp_send_control_init(&control, 4u, UINT32_C(0x01), 16u, 100000u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_calculate_handshake_delay(&control) == 150000u);
    REQUIRE(utp_send_control_calculate_handshake_delay(&control) == 300000u);
    REQUIRE(utp_send_control_calculate_rto(&control) == 500000u);
    REQUIRE(utp_rtt_stats_update(&control.rtt_stats, 10000u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_calculate_tlp_delay(&control) == 115000u);
    REQUIRE(utp_send_control_calculate_rto(&control) == 200000u);

    control.consecutive_rto_count = 10u;
    REQUIRE(utp_send_control_calculate_rto(&control) == 60000000u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control prioritizes handshake, loss, TLP, and RTO retransmission modes", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   packet  = make_packet(1u, 100u);

    packet.frame_types = UINT32_C(0x01);
    packet.po_flags    = UTP_PO_HELLO;
    REQUIRE(utp_send_control_init(&control, 4u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_retransmission_mode(&control) == UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE);

    utp_send_control_set_connected(&control, true);
    utp_send_control_set_loss_pending(&control, true);
    REQUIRE(utp_send_control_retransmission_mode(&control) == UTP_SEND_CONTROL_RETRANSMISSION_LOSS);
    utp_send_control_set_loss_pending(&control, false);
    REQUIRE(utp_send_control_retransmission_mode(&control) == UTP_SEND_CONTROL_RETRANSMISSION_TLP);
    control.tlp_count = 2u;
    REQUIRE(utp_send_control_retransmission_mode(&control) == UTP_SEND_CONTROL_RETRANSMISSION_RTO);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control timeout moves every retransmittable packet to the loss queue", "[send_control]")
{
    utp_send_control_t control = {};
    utp_packet_out_t   first   = make_packet(1u, 100u);
    utp_packet_out_t   second  = make_packet(2u, 200u);

    first.frame_types  = UINT32_C(0x01);
    second.frame_types = UINT32_C(0x01);
    REQUIRE(utp_send_control_init(&control, 4u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &second) == UTP_INTERNAL_ERROR_OK);
    utp_send_control_set_connected(&control, true);
    control.tlp_count = 2u;

    REQUIRE(utp_send_control_on_retransmission_timeout(&control) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(control.consecutive_rto_count == 1u);
    REQUIRE(utp_send_control_unacked_packet_count(&control) == 0u);
    REQUIRE(utp_send_control_lost_packet_count(&control) == 2u);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control resets retransmission backoff after ACK progress", "[send_control]")
{
    utp_send_control_t            control = {};
    utp_packet_out_t              packet  = make_packet(1u, 100u);
    struct utp_packet_out_tailq   acknowledged;
    utp_ack_range_t               ranges[] = {{1u, 1u}};
    const utp_ack_info_t          ack      = {1u, 0u, ranges, 1u, 1u};
    utp_send_control_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_on_packet_sent(&control, &packet) == UTP_INTERNAL_ERROR_OK);
    control.consecutive_rto_count          = 3u;
    control.handshake_retransmission_count = 2u;
    control.tlp_count                      = 1u;
    control.loss_pending                   = true;

    REQUIRE(utp_send_control_on_ack(&control, &ack, 1000u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(control.consecutive_rto_count == 0u);
    REQUIRE(control.handshake_retransmission_count == 0u);
    REQUIRE(control.tlp_count == 0u);
    REQUIRE_FALSE(control.loss_pending);
    utp_send_control_cleanup(&control);
}

TEST_CASE("send control allocates packet numbers without reuse", "[send_control]")
{
    utp_send_control_t control = {};
    uint64_t           first   = 0u;
    uint64_t           second  = 0u;

    REQUIRE(utp_send_control_init(&control, 2u, UINT32_C(0x01), 16u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_allocate_packet_number(&control, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_allocate_packet_number(&control, &second) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(first == 1u);
    REQUIRE(second == 2u);
    utp_send_control_cleanup(&control);
}
