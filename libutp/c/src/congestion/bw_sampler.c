#include "congestion/bw_sampler.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>

static uint64_t utp_bw_rate(uint64_t bytes, uint64_t interval_us)
{
    return interval_us == 0u || bytes > UINT64_MAX / UINT64_C(1000000) ? 0u : bytes * UINT64_C(1000000) / interval_us;
}

void utp_bw_sampler_init(utp_bw_sampler_t* sampler)
{
    assert(sampler != NULL);
    sampler->total_bytes_sent                = 0u;
    sampler->total_bytes_acked               = 0u;
    sampler->total_bytes_lost                = 0u;
    sampler->last_acked_total_sent           = 0u;
    sampler->last_acked_sent_time_us         = 0u;
    sampler->last_acked_time_us              = 0u;
    sampler->last_sent_packet_number         = 0u;
    sampler->app_limited_until_packet_number = 0u;
    sampler->app_limited                     = true;
}

void utp_bw_sampler_on_packet_sent(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet, uint64_t packet_number,
                                   uint32_t packet_size, uint64_t sent_time_us)
{
    assert(sampler != NULL);
    if (packet == NULL || packet_number == 0u || packet_size == 0u) {
        return;
    }
    packet->total_bytes_sent  = sampler->total_bytes_sent;
    packet->total_bytes_acked = sampler->total_bytes_acked;
    packet->total_bytes_lost  = sampler->total_bytes_lost;
    packet->sent_at_last_ack  = sampler->last_acked_total_sent;
    packet->last_ack_sent_time_us =
        sampler->last_acked_sent_time_us == 0u ? sent_time_us : sampler->last_acked_sent_time_us;
    packet->last_ack_time_us          = sampler->last_acked_time_us;
    packet->packet_number             = packet_number;
    packet->sent_time_us              = sent_time_us;
    packet->packet_size               = packet_size;
    packet->app_limited               = sampler->app_limited;
    packet->valid                     = true;
    sampler->total_bytes_sent        += packet_size;
    sampler->last_sent_packet_number  = packet_number;
}

bool utp_bw_sampler_on_packet_acked(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet, uint64_t packet_number,
                                    uint64_t ack_time_us, utp_bw_sample_t* sample)
{
    assert(sampler != NULL);
    assert(sample != NULL);
    if (packet == NULL || !packet->valid || packet->packet_number != packet_number ||
        ack_time_us <= packet->last_ack_time_us) {
        return false;
    }
    uint64_t delivered;
    uint64_t interval;

    sample->bandwidth_bytes_per_second  = 0u;
    sample->rtt_us                      = 0u;
    sample->is_app_limited              = false;
    sampler->total_bytes_acked         += packet->packet_size;
    if (sampler->app_limited && packet_number > sampler->app_limited_until_packet_number) {
        sampler->app_limited = false;
    }
    delivered                          = sampler->total_bytes_acked - packet->total_bytes_acked;
    interval                           = ack_time_us - packet->last_ack_time_us;
    sample->bandwidth_bytes_per_second = utp_bw_rate(delivered, interval);
    sample->rtt_us                     = ack_time_us - packet->sent_time_us;
    sample->is_app_limited             = packet->app_limited;
    sampler->last_acked_total_sent     = packet->total_bytes_sent;
    sampler->last_acked_sent_time_us   = packet->last_ack_sent_time_us;
    sampler->last_acked_time_us        = ack_time_us;
    packet->valid                      = false;
    return sample->rtt_us != 0u;
}

void utp_bw_sampler_on_packet_lost(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet)
{
    assert(sampler != NULL);
    if (packet != NULL && packet->valid) {
        sampler->total_bytes_lost += packet->packet_size;
        packet->valid              = false;
    }
}

void utp_bw_sampler_set_app_limited(utp_bw_sampler_t* sampler)
{
    assert(sampler != NULL);
    sampler->app_limited                     = true;
    sampler->app_limited_until_packet_number = sampler->last_sent_packet_number;
}
