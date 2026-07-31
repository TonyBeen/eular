#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/congestion.h"
#include "congestion/cubic.h"
#include "congestion/pacer.h"
#include "util/rtt.h"
}

namespace {

struct CongestionTrace {
    uint64_t cwnd          = 0u;
    uint64_t pacing_rate   = 0u;
    uint64_t last_inflight = 0u;
    uint64_t last_now      = 0u;
    uint64_t last_ack_now  = 0u;
    uint32_t calls         = 0u;
};

void trace_init(void* state, const utp_rtt_stats_t* rtt_stats) {
    auto* trace = static_cast<CongestionTrace*>(state);

    REQUIRE(rtt_stats != nullptr);
    ++trace->calls;
}

uint64_t trace_get_cwnd(void* state) { return static_cast<CongestionTrace*>(state)->cwnd; }

uint64_t trace_get_pacing_rate(void* state, int32_t in_recovery) {
    auto* trace = static_cast<CongestionTrace*>(state);

    return in_recovery != 0 ? trace->pacing_rate / 2u : trace->pacing_rate;
}

void trace_begin_ack(void* state, uint64_t now_us, uint64_t inflight_bytes) {
    auto* trace = static_cast<CongestionTrace*>(state);

    trace->last_now      = now_us;
    trace->last_inflight = inflight_bytes;
    ++trace->calls;
}

void trace_packet(void* state, utp_congestion_packet_info_t* packet, uint64_t now_us, int32_t app_limited) {
    auto* trace = static_cast<CongestionTrace*>(state);

    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 7u);
    REQUIRE(packet->packet_size == 1200u);
    REQUIRE(app_limited == 0);
    packet->state   = trace;
    trace->last_now = now_us;
    if (now_us != 0u) {
        trace->last_ack_now = now_us;
    }
    ++trace->calls;
}

void trace_packet_sent(void* state, utp_congestion_packet_info_t* packet, uint64_t inflight_bytes,
                       int32_t app_limited) {
    trace_packet(state, packet, 0u, app_limited);
    static_cast<CongestionTrace*>(state)->last_inflight = inflight_bytes;
}

void trace_lost(void* state, utp_congestion_packet_info_t* packet) { trace_packet(state, packet, 0u, 0); }

void trace_was_quiet(void* state, uint64_t now_us, uint64_t inflight_bytes) {
    trace_begin_ack(state, now_us, inflight_bytes);
}

void trace_end_ack(void* state, uint64_t inflight_bytes) {
    auto* trace = static_cast<CongestionTrace*>(state);

    trace->last_inflight = inflight_bytes;
    ++trace->calls;
}

void trace_event(void* state) { ++static_cast<CongestionTrace*>(state)->calls; }

const utp_congestion_ops_t kTraceOps = {
    trace_init, trace_get_cwnd,  trace_get_pacing_rate, trace_begin_ack, trace_packet_sent, trace_packet,
    trace_lost, trace_was_quiet, trace_end_ack,         trace_event,     trace_event,
};

}  // namespace

TEST_CASE("pacer consumes burst tokens then reports its pacing deadline", "[congestion][pacer]") {
    utp_pacer_t pacer = {};

    utp_pacer_init(&pacer, 100u);
    utp_pacer_tick_in(&pacer, 1000u);
    for (uint32_t index = 0u; index < UTP_PACER_BURST_TOKENS; ++index) {
        REQUIRE(utp_pacer_can_schedule(&pacer, 1u));
        utp_pacer_packet_scheduled(&pacer, 1u, false, 200u);
    }
    utp_pacer_packet_scheduled(&pacer, 1u, false, 200u);
    REQUIRE_FALSE(utp_pacer_can_schedule(&pacer, 1u));
    REQUIRE(utp_pacer_delayed(&pacer));

    utp_pacer_tick_in(&pacer, 1200u);
    REQUIRE(utp_pacer_can_schedule(&pacer, 1u));
    REQUIRE(utp_pacer_next_scheduled_time(&pacer) == 1200u);
}

TEST_CASE("congestion dispatches complete packet and transaction events", "[congestion]") {
    CongestionTrace              trace      = {};
    utp_congestion_t             congestion = {&trace, &kTraceOps};
    utp_rtt_stats_t              rtt_stats  = {};
    utp_congestion_packet_info_t packet     = {7u, 100u, 1200u, nullptr};

    trace.cwnd        = 32000u;
    trace.pacing_rate = 64000u;

    utp_congestion_init(&congestion, &rtt_stats);
    REQUIRE(utp_congestion_get_cwnd(&congestion) == 32000u);
    REQUIRE(utp_congestion_get_pacing_rate(&congestion, 0) == 64000u);
    REQUIRE(utp_congestion_get_pacing_rate(&congestion, 1) == 32000u);

    utp_congestion_was_quiet(&congestion, 200u, 0u);
    utp_congestion_on_begin_ack(&congestion, 300u, 1200u);
    utp_congestion_on_packet_sent(&congestion, &packet, 1200u, 0);
    utp_congestion_on_ack(&congestion, &packet, 400u, 0);
    utp_congestion_on_lost(&congestion, &packet);
    utp_congestion_on_end_ack(&congestion, 0u);
    utp_congestion_on_loss(&congestion);
    utp_congestion_on_timeout(&congestion);

    REQUIRE(packet.state == &trace);
    REQUIRE(trace.last_ack_now == 400u);
    REQUIRE(trace.last_inflight == 0u);
    REQUIRE(trace.calls == 9u);
}

TEST_CASE("cubic performs slow start, loss recovery, and timeout backoff", "[congestion][cubic]") {
    utp_cubic_t                  cubic     = {};
    utp_rtt_stats_t              rtt_stats = {};
    utp_congestion_packet_info_t packet    = {1u, 100u, UTP_CUBIC_DEFAULT_MSS, nullptr};
    utp_congestion_t*            congestion;

    REQUIRE(utp_rtt_stats_update(&rtt_stats, 10000u) == UTP_INTERNAL_ERROR_OK);
    utp_cubic_init(&cubic, nullptr);
    congestion = utp_cubic_as_congestion(&cubic);
    utp_congestion_init(congestion, &rtt_stats);

    REQUIRE(utp_congestion_get_cwnd(congestion) == 32u * UTP_CUBIC_DEFAULT_MSS);
    REQUIRE(utp_congestion_get_pacing_rate(congestion, 0) == 5840000u);
    REQUIRE(utp_congestion_get_pacing_rate(congestion, 1) == 4672000u);

    utp_congestion_on_ack(congestion, &packet, 20000u, 0);
    REQUIRE(utp_congestion_get_cwnd(congestion) == 33u * UTP_CUBIC_DEFAULT_MSS);

    utp_congestion_on_lost(congestion, &packet);
    REQUIRE(utp_congestion_get_cwnd(congestion) == 33726u);

    utp_congestion_on_timeout(congestion);
    REQUIRE(utp_congestion_get_cwnd(congestion) == 4u * UTP_CUBIC_DEFAULT_MSS);
}
