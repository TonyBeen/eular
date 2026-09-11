#include "mtu/mtu.h"

#include <assert.h>
#include <limits.h>

#include "proto/proto.h"

#define UTP_MTU_DEFAULT_PROBE_INTERVAL_MS        300000u
#define UTP_MTU_DEFAULT_PROBE_TIMEOUT_MS         2000u
#define UTP_MTU_DEFAULT_BLACKHOLE_LOSS_THRESHOLD 3u
#define UTP_MTU_DEFAULT_BLACKHOLE_LOSS_WINDOW_MS 3000u
#define UTP_MTU_DEFAULT_BLACKHOLE_COOLDOWN_MS    5000u
#define UTP_MTU_IPV4_HEADER_SIZE                 20u
#define UTP_MTU_IPV6_HEADER_SIZE                 40u
#define UTP_MTU_UDP_HEADER_SIZE                  8u

static const uint16_t utp_mtu_probe_ladder[] = {1380u, 1450u, 1492u, 1500u};

static uint16_t       utp_mtu_clamp(uint16_t value, uint16_t minimum, uint16_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static uint64_t utp_mtu_add_ms(uint64_t now_ms, uint64_t delay_ms)
{
    return now_ms > UINT64_MAX - delay_ms ? UINT64_MAX : now_ms + delay_ms;
}

static uint16_t utp_mtu_next_ladder_target(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    for (size_t index = 0u; index < sizeof(utp_mtu_probe_ladder) / sizeof(utp_mtu_probe_ladder[0]); ++index) {
        const uint16_t candidate = utp_mtu_probe_ladder[index];

        if (candidate > discovery->search_low_mtu && candidate <= discovery->ceiling_mtu &&
            candidate <= discovery->mtu_max && candidate >= discovery->mtu_min) {
            return candidate;
        }
    }
    return discovery->search_low_mtu;
}

static uint16_t utp_mtu_next_binary_target(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    if ((uint32_t)discovery->search_high_mtu <= (uint32_t)discovery->search_low_mtu + discovery->probe_step) {
        /* 最后一次直接确认配置上限，避免 probe_step 让 mtu_max 永远不被探测。 */
        if (discovery->search_high_mtu == discovery->mtu_max) {
            return discovery->search_high_mtu;
        }
        return discovery->search_low_mtu;
    }
    uint16_t low = (uint16_t)(discovery->search_low_mtu + 1u);
    if (low >= discovery->search_high_mtu) {
        return discovery->search_low_mtu;
    }
    uint16_t candidate = (uint16_t)(low + (uint16_t)((discovery->search_high_mtu - low) / 2u));
    return candidate <= discovery->search_low_mtu ? discovery->search_low_mtu : candidate;
}

static void utp_mtu_clear_in_flight_probe(utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    discovery->has_in_flight_probe           = false;
    discovery->in_flight_probe_packet_number = 0u;
    discovery->in_flight_probe_mtu           = 0u;
    discovery->in_flight_probe_deadline_ms   = 0u;
}

static void utp_mtu_clear_probe_retry(utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    discovery->retry_pending     = false;
    discovery->retry_probe_mtu   = 0u;
    discovery->probe_retry_count = 0u;
}

static bool utp_mtu_discovery_record_probe_failure(utp_mtu_discovery_t* discovery, uint16_t probe_mtu, uint64_t now_ms)
{
    assert(discovery != NULL);
    assert(probe_mtu > discovery->search_low_mtu);
    const uint16_t high = (uint16_t)(probe_mtu - 1u);

    utp_mtu_clear_probe_retry(discovery);
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE) {
        discovery->current_mtu     = discovery->mtu_min;
        discovery->search_low_mtu  = discovery->mtu_min;
        discovery->search_high_mtu = high < discovery->mtu_min ? discovery->mtu_min : high;
        discovery->ceiling_mtu     = discovery->search_high_mtu;
        discovery->probe_phase     = UTP_MTU_PROBE_PHASE_BINARY;
        if ((uint32_t)discovery->search_high_mtu <= (uint32_t)discovery->search_low_mtu + discovery->probe_step) {
            discovery->ceiling_mtu        = discovery->current_mtu;
            discovery->search_high_mtu    = discovery->current_mtu;
            discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
            discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, discovery->probe_interval_ms);
        } else {
            discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, 1u);
        }
        return true;
    }
    if (high < discovery->search_high_mtu) {
        discovery->search_high_mtu = high;
    }
    if (discovery->search_high_mtu < discovery->ceiling_mtu) {
        discovery->ceiling_mtu = discovery->search_high_mtu;
    }
    discovery->probe_phase = UTP_MTU_PROBE_PHASE_BINARY;
    if ((uint32_t)discovery->search_high_mtu <= (uint32_t)discovery->search_low_mtu + discovery->probe_step) {
        discovery->current_mtu        = discovery->search_low_mtu;
        discovery->ceiling_mtu        = discovery->search_low_mtu;
        discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, discovery->probe_interval_ms);
    } else {
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, 1u);
    }
    return true;
}

uint16_t utp_mtu_minimum_supported(uint8_t family)
{
    const uint16_t ip_header = family == UTP_ADDRESS_FAMILY_IPV6 ? UTP_MTU_IPV6_HEADER_SIZE : UTP_MTU_IPV4_HEADER_SIZE;

    return (uint16_t)(ip_header + UTP_MTU_UDP_HEADER_SIZE + UTP_PACKET_HEADER_SIZE + 1u);
}

uint16_t utp_mtu_normalize(uint16_t mtu, uint8_t family)
{
    return utp_mtu_clamp(mtu, utp_mtu_minimum_supported(family), UTP_MTU_MAX);
}

uint16_t utp_mtu_packet_size_from_mtu(uint16_t mtu, uint8_t family)
{
    const uint16_t ip_header = family == UTP_ADDRESS_FAMILY_IPV6 ? UTP_MTU_IPV6_HEADER_SIZE : UTP_MTU_IPV4_HEADER_SIZE;
    const uint16_t overhead  = (uint16_t)(ip_header + UTP_MTU_UDP_HEADER_SIZE);

    return mtu <= overhead ? 0u : (uint16_t)(mtu - overhead);
}

uint16_t utp_mtu_from_packet_size(uint16_t packet_size, uint8_t family)
{
    const uint16_t ip_header = family == UTP_ADDRESS_FAMILY_IPV6 ? UTP_MTU_IPV6_HEADER_SIZE : UTP_MTU_IPV4_HEADER_SIZE;

    return (uint16_t)(packet_size + ip_header + UTP_MTU_UDP_HEADER_SIZE);
}

void utp_mtu_discovery_init(utp_mtu_discovery_t* discovery, const utp_mtu_config_t* config, uint8_t family)
{
    const uint16_t configured_min  = config == NULL ? UTP_MTU_DEFAULT_MIN : config->mtu_min;
    const uint16_t configured_max  = config == NULL ? UTP_MTU_ETHERNET_MAX : config->mtu_max;
    const uint16_t configured_base = config == NULL ? UTP_MTU_DEFAULT_BASE : config->mtu_base;
    uint64_t       configured_interval_ms;

    assert(discovery != NULL);
    discovery->enabled  = config == NULL || config->enabled;
    discovery->family   = family == UTP_ADDRESS_FAMILY_IPV6 ? UTP_ADDRESS_FAMILY_IPV6 : UTP_ADDRESS_FAMILY_IPV4;
    discovery->mtu_min  = utp_mtu_normalize(configured_min, discovery->family);
    discovery->mtu_max  = utp_mtu_clamp(configured_max, discovery->mtu_min, UTP_MTU_MAX);
    discovery->mtu_base = utp_mtu_clamp(configured_base, discovery->mtu_min, discovery->mtu_max);
    discovery->probe_step =
        config == NULL || config->probe_step == 0u ? UTP_MTU_DEFAULT_PROBE_STEP : config->probe_step;
    discovery->probe_timeout_ms =
        config == NULL || config->probe_timeout_ms == 0u ? UTP_MTU_DEFAULT_PROBE_TIMEOUT_MS : config->probe_timeout_ms;
    discovery->probe_retries            = config == NULL ? 1u : config->probe_retries;
    discovery->blackhole_loss_threshold = config == NULL || config->blackhole_loss_threshold == 0u
                                              ? UTP_MTU_DEFAULT_BLACKHOLE_LOSS_THRESHOLD
                                              : config->blackhole_loss_threshold;
    discovery->blackhole_loss_window_ms = config == NULL || config->blackhole_loss_window_ms == 0u
                                              ? UTP_MTU_DEFAULT_BLACKHOLE_LOSS_WINDOW_MS
                                              : config->blackhole_loss_window_ms;
    discovery->blackhole_cooldown_ms    = config == NULL || config->blackhole_cooldown_ms == 0u
                                              ? UTP_MTU_DEFAULT_BLACKHOLE_COOLDOWN_MS
                                              : config->blackhole_cooldown_ms;
    configured_interval_ms =
        config == NULL ? UTP_MTU_DEFAULT_PROBE_INTERVAL_MS : (uint64_t)config->probe_interval_seconds * UINT64_C(1000);
    discovery->probe_interval_ms           = configured_interval_ms < 1000u        ? 1000u
                                             : configured_interval_ms > UINT32_MAX ? UINT32_MAX
                                                                                   : (uint32_t)configured_interval_ms;
    discovery->current_mtu                 = discovery->mtu_base;
    discovery->ceiling_mtu                 = discovery->mtu_max;
    discovery->search_low_mtu              = discovery->current_mtu;
    discovery->search_high_mtu             = discovery->ceiling_mtu;
    discovery->probe_phase                 = UTP_MTU_PROBE_PHASE_LADDER;
    discovery->next_probe_time_ms          = 0u;
    discovery->last_large_ack_ms           = 0u;
    discovery->last_large_loss_ms          = 0u;
    discovery->blackhole_cooldown_until_ms = 0u;
    discovery->large_loss_streak           = 0u;
    utp_mtu_clear_in_flight_probe(discovery);
    utp_mtu_clear_probe_retry(discovery);
}

void utp_mtu_discovery_on_path_validated(utp_mtu_discovery_t* discovery, uint64_t now_ms)
{
    assert(discovery != NULL);
    discovery->current_mtu                 = discovery->mtu_base;
    discovery->last_large_ack_ms           = 0u;
    discovery->last_large_loss_ms          = 0u;
    discovery->large_loss_streak           = 0u;
    discovery->blackhole_cooldown_until_ms = 0u;
    utp_mtu_clear_in_flight_probe(discovery);
    utp_mtu_clear_probe_retry(discovery);
    if (!discovery->enabled) {
        discovery->ceiling_mtu        = discovery->current_mtu;
        discovery->search_low_mtu     = discovery->current_mtu;
        discovery->search_high_mtu    = discovery->current_mtu;
        discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
        discovery->next_probe_time_ms = now_ms;
        return;
    }
    discovery->ceiling_mtu        = discovery->mtu_max;
    discovery->search_low_mtu     = discovery->current_mtu;
    discovery->search_high_mtu    = discovery->ceiling_mtu;
    discovery->probe_phase        = UTP_MTU_PROBE_PHASE_LADDER;
    discovery->next_probe_time_ms = now_ms;
}

void utp_mtu_discovery_set_address_family(utp_mtu_discovery_t* discovery, uint8_t family)
{
    assert(discovery != NULL);
    if (family == UTP_ADDRESS_FAMILY_IPV4 || family == UTP_ADDRESS_FAMILY_IPV6) {
        discovery->family = family;
    }
}

bool utp_mtu_discovery_enabled(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    return discovery->enabled;
}

bool utp_mtu_discovery_has_in_flight_probe(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    return discovery->has_in_flight_probe;
}

uint16_t utp_mtu_discovery_path_mtu(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    return discovery->current_mtu;
}

uint16_t utp_mtu_discovery_next_probe_mtu(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    if (!discovery->enabled) {
        return discovery->search_low_mtu;
    }
    if (discovery->retry_pending) {
        return discovery->retry_probe_mtu;
    }
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE) {
        return discovery->mtu_base > discovery->search_low_mtu ? discovery->mtu_base : discovery->search_low_mtu;
    }
    if (discovery->search_low_mtu >= discovery->ceiling_mtu) {
        return discovery->search_low_mtu;
    }
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_LADDER) {
        uint16_t candidate = utp_mtu_next_ladder_target(discovery);
        return candidate > discovery->search_low_mtu ? candidate : utp_mtu_next_binary_target(discovery);
    }
    return discovery->probe_phase == UTP_MTU_PROBE_PHASE_BINARY ? utp_mtu_next_binary_target(discovery)
                                                                : discovery->search_low_mtu;
}

uint16_t utp_mtu_discovery_current_max_packet_size(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    return utp_mtu_packet_size_from_mtu(discovery->current_mtu, discovery->family);
}

uint16_t utp_mtu_discovery_absolute_max_packet_size(const utp_mtu_discovery_t* discovery)
{
    assert(discovery != NULL);
    return utp_mtu_packet_size_from_mtu(discovery->ceiling_mtu, discovery->family);
}

bool utp_mtu_discovery_should_probe(const utp_mtu_discovery_t* discovery, uint64_t now_ms)
{
    assert(discovery != NULL);
    return discovery->enabled && !discovery->has_in_flight_probe && now_ms >= discovery->blackhole_cooldown_until_ms &&
           now_ms >= discovery->next_probe_time_ms &&
           utp_mtu_discovery_next_probe_mtu(discovery) > discovery->search_low_mtu;
}

bool utp_mtu_discovery_on_probe_sent(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint16_t probe_mtu,
                                     uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->enabled || packet_number == 0u || discovery->search_low_mtu >= discovery->ceiling_mtu) {
        return false;
    }
    uint16_t clamped_probe =
        utp_mtu_clamp(probe_mtu, (uint16_t)(discovery->search_low_mtu + 1u), discovery->ceiling_mtu);
    if (clamped_probe <= discovery->search_low_mtu) {
        return false;
    }
    if (discovery->retry_pending && clamped_probe != discovery->retry_probe_mtu) {
        return false;
    }
    if (discovery->retry_pending) {
        discovery->retry_pending = false;
    } else {
        discovery->retry_probe_mtu   = 0u;
        discovery->probe_retry_count = 0u;
    }
    uint16_t ladder_target = utp_mtu_next_ladder_target(discovery);
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_LADDER &&
        (ladder_target == discovery->search_low_mtu || clamped_probe != ladder_target)) {
        discovery->probe_phase = UTP_MTU_PROBE_PHASE_BINARY;
    }
    discovery->has_in_flight_probe           = true;
    discovery->in_flight_probe_packet_number = packet_number;
    discovery->in_flight_probe_mtu           = clamped_probe;
    discovery->in_flight_probe_deadline_ms   = utp_mtu_add_ms(now_ms, discovery->probe_timeout_ms);
    return true;
}

bool utp_mtu_discovery_on_probe_ack(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->has_in_flight_probe || packet_number != discovery->in_flight_probe_packet_number) {
        return false;
    }
    if (discovery->in_flight_probe_mtu > discovery->search_low_mtu) {
        discovery->search_low_mtu = discovery->in_flight_probe_mtu;
    }
    discovery->last_large_ack_ms = now_ms;
    discovery->large_loss_streak = 0u;
    utp_mtu_clear_in_flight_probe(discovery);
    utp_mtu_clear_probe_retry(discovery);
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE) {
        discovery->current_mtu     = discovery->mtu_base;
        discovery->ceiling_mtu     = discovery->mtu_max;
        discovery->search_low_mtu  = discovery->current_mtu;
        discovery->search_high_mtu = discovery->ceiling_mtu;
        if (discovery->current_mtu >= discovery->ceiling_mtu) {
            discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
            discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, discovery->probe_interval_ms);
        } else {
            discovery->probe_phase        = UTP_MTU_PROBE_PHASE_LADDER;
            discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, 1u);
        }
        return true;
    }
    if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_LADDER &&
        utp_mtu_next_ladder_target(discovery) <= discovery->search_low_mtu) {
        discovery->probe_phase = UTP_MTU_PROBE_PHASE_BINARY;
    }
    if (discovery->search_low_mtu >= discovery->mtu_max) {
        discovery->current_mtu        = discovery->mtu_max;
        discovery->ceiling_mtu        = discovery->mtu_max;
        discovery->search_low_mtu     = discovery->current_mtu;
        discovery->search_high_mtu    = discovery->current_mtu;
        discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, discovery->probe_interval_ms);
    } else if (discovery->probe_phase == UTP_MTU_PROBE_PHASE_BINARY &&
               (uint32_t)discovery->search_high_mtu <= (uint32_t)discovery->search_low_mtu + discovery->probe_step &&
               discovery->search_high_mtu != discovery->mtu_max) {
        discovery->current_mtu        = discovery->search_low_mtu;
        discovery->ceiling_mtu        = discovery->search_low_mtu;
        discovery->probe_phase        = UTP_MTU_PROBE_PHASE_STABLE;
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, discovery->probe_interval_ms);
    } else {
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, 1u);
    }
    return true;
}

bool utp_mtu_discovery_on_probe_lost(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->has_in_flight_probe || packet_number != discovery->in_flight_probe_packet_number) {
        return false;
    }
    uint16_t probe_mtu = discovery->in_flight_probe_mtu;
    utp_mtu_clear_in_flight_probe(discovery);
    if (probe_mtu > discovery->search_low_mtu && discovery->probe_retry_count < discovery->probe_retries) {
        ++discovery->probe_retry_count;
        discovery->retry_probe_mtu    = probe_mtu;
        discovery->retry_pending      = true;
        discovery->next_probe_time_ms = utp_mtu_add_ms(now_ms, 1u);
        return true;
    }
    return probe_mtu > discovery->search_low_mtu &&
           utp_mtu_discovery_record_probe_failure(discovery, probe_mtu, now_ms);
}

bool utp_mtu_discovery_on_probe_send_failed(utp_mtu_discovery_t* discovery, uint16_t probe_mtu, uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->enabled || discovery->has_in_flight_probe || discovery->search_low_mtu >= discovery->ceiling_mtu) {
        return false;
    }
    uint16_t clamped_probe =
        utp_mtu_clamp(probe_mtu, (uint16_t)(discovery->search_low_mtu + 1u), discovery->ceiling_mtu);
    return clamped_probe > discovery->search_low_mtu &&
           utp_mtu_discovery_record_probe_failure(discovery, clamped_probe, now_ms);
}

bool utp_mtu_discovery_on_probe_timeout(utp_mtu_discovery_t* discovery, uint64_t now_ms)
{
    assert(discovery != NULL);
    return discovery->has_in_flight_probe && now_ms >= discovery->in_flight_probe_deadline_ms &&
           utp_mtu_discovery_on_probe_lost(discovery, discovery->in_flight_probe_packet_number, now_ms);
}

bool utp_mtu_discovery_on_data_packet_ack(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->enabled ||
        (uint32_t)packet_size + discovery->probe_step < utp_mtu_discovery_current_max_packet_size(discovery)) {
        return false;
    }
    discovery->last_large_ack_ms = now_ms;
    discovery->large_loss_streak = 0u;
    return true;
}

bool utp_mtu_discovery_on_data_packet_loss(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms)
{
    assert(discovery != NULL);
    if (!discovery->enabled ||
        (uint32_t)packet_size + discovery->probe_step < utp_mtu_discovery_current_max_packet_size(discovery)) {
        return false;
    }
    if (discovery->last_large_loss_ms == 0u ||
        now_ms > utp_mtu_add_ms(discovery->last_large_loss_ms, discovery->blackhole_loss_window_ms)) {
        discovery->large_loss_streak = 0u;
    }
    discovery->last_large_loss_ms = now_ms;
    if (discovery->large_loss_streak != UINT8_MAX) {
        ++discovery->large_loss_streak;
    }
    if (discovery->large_loss_streak < discovery->blackhole_loss_threshold ||
        (discovery->last_large_ack_ms != 0u &&
         now_ms <= utp_mtu_add_ms(discovery->last_large_ack_ms, (uint64_t)discovery->probe_timeout_ms * 2u))) {
        return false;
    }
    discovery->current_mtu     = discovery->mtu_min;
    discovery->ceiling_mtu     = discovery->mtu_max;
    discovery->search_low_mtu  = discovery->mtu_min;
    discovery->search_high_mtu = discovery->ceiling_mtu;
    discovery->probe_phase =
        discovery->mtu_base > discovery->mtu_min ? UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE : UTP_MTU_PROBE_PHASE_LADDER;
    utp_mtu_clear_in_flight_probe(discovery);
    utp_mtu_clear_probe_retry(discovery);
    discovery->blackhole_cooldown_until_ms = utp_mtu_add_ms(now_ms, discovery->blackhole_cooldown_ms);
    discovery->next_probe_time_ms          = discovery->blackhole_cooldown_until_ms;
    discovery->large_loss_streak           = 0u;
    return true;
}
