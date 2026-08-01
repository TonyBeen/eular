#ifndef EULAR_UTP_INTERNAL_MTU_H
#define EULAR_UTP_INTERNAL_MTU_H

#include <stdbool.h>
#include <stdint.h>

#include "socket/address.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_MTU_MAX                UINT16_MAX
#define UTP_MTU_ETHERNET_MAX       1500u
#define UTP_MTU_DEFAULT_MIN        1280u
#define UTP_MTU_DEFAULT_BASE       1400u
#define UTP_MTU_DEFAULT_PROBE_STEP 16u

typedef enum utp_mtu_probe_phase {
    UTP_MTU_PROBE_PHASE_LADDER = 0,
    UTP_MTU_PROBE_PHASE_BINARY,
    UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE,
    UTP_MTU_PROBE_PHASE_STABLE
} utp_mtu_probe_phase_t;

typedef struct utp_mtu_config {
    bool     enabled;
    uint16_t mtu_min;
    uint16_t mtu_max;
    uint16_t mtu_base;
    uint32_t probe_interval_seconds;
    uint16_t probe_step;
    uint16_t probe_timeout_ms;
    uint8_t  probe_retries;
    uint8_t  blackhole_loss_threshold;
    uint16_t blackhole_loss_window_ms;
    uint16_t blackhole_cooldown_ms;
} utp_mtu_config_t;

#define UTP_MTU_CONFIG_INIT {true, 1280u, 1500u, 1400u, 300u, 16u, 2000u, 1u, 3u, 3000u, 5000u}

typedef struct utp_mtu_discovery {
    uint64_t              next_probe_time_ms;
    uint64_t              last_large_ack_ms;
    uint64_t              last_large_loss_ms;
    uint64_t              blackhole_cooldown_until_ms;
    uint64_t              in_flight_probe_deadline_ms;
    uint64_t              in_flight_probe_packet_number;
    uint32_t              probe_interval_ms;
    uint16_t              mtu_min;
    uint16_t              mtu_max;
    uint16_t              mtu_base;
    uint16_t              probe_step;
    uint16_t              probe_timeout_ms;
    uint16_t              blackhole_loss_window_ms;
    uint16_t              blackhole_cooldown_ms;
    uint16_t              current_mtu;
    uint16_t              ceiling_mtu;
    uint16_t              search_low_mtu;
    uint16_t              search_high_mtu;
    uint16_t              in_flight_probe_mtu;
    uint16_t              retry_probe_mtu;
    uint8_t               blackhole_loss_threshold;
    uint8_t               large_loss_streak;
    uint8_t               probe_retries;
    uint8_t               probe_retry_count;
    uint8_t               family;
    bool                  enabled;
    bool                  has_in_flight_probe;
    bool                  retry_pending;
    utp_mtu_probe_phase_t probe_phase;
} utp_mtu_discovery_t;

void     utp_mtu_discovery_init(utp_mtu_discovery_t* discovery, const utp_mtu_config_t* config, uint8_t family);
void     utp_mtu_discovery_on_path_validated(utp_mtu_discovery_t* discovery, uint64_t now_ms);
void     utp_mtu_discovery_set_address_family(utp_mtu_discovery_t* discovery, uint8_t family);
bool     utp_mtu_discovery_enabled(const utp_mtu_discovery_t* discovery);
bool     utp_mtu_discovery_has_in_flight_probe(const utp_mtu_discovery_t* discovery);
uint16_t utp_mtu_discovery_path_mtu(const utp_mtu_discovery_t* discovery);
uint16_t utp_mtu_discovery_next_probe_mtu(const utp_mtu_discovery_t* discovery);
uint16_t utp_mtu_discovery_current_max_packet_size(const utp_mtu_discovery_t* discovery);
uint16_t utp_mtu_discovery_absolute_max_packet_size(const utp_mtu_discovery_t* discovery);
bool     utp_mtu_discovery_should_probe(const utp_mtu_discovery_t* discovery, uint64_t now_ms);
bool     utp_mtu_discovery_on_probe_sent(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint16_t probe_mtu,
                                         uint64_t now_ms);
bool     utp_mtu_discovery_on_probe_ack(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms);
bool     utp_mtu_discovery_on_probe_lost(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms);
bool     utp_mtu_discovery_on_probe_send_failed(utp_mtu_discovery_t* discovery, uint16_t probe_mtu, uint64_t now_ms);
bool     utp_mtu_discovery_on_probe_timeout(utp_mtu_discovery_t* discovery, uint64_t now_ms);
bool     utp_mtu_discovery_on_data_packet_ack(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms);
bool     utp_mtu_discovery_on_data_packet_loss(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms);
uint16_t utp_mtu_minimum_supported(uint8_t family);
uint16_t utp_mtu_normalize(uint16_t mtu, uint8_t family);
uint16_t utp_mtu_packet_size_from_mtu(uint16_t mtu, uint8_t family);
uint16_t utp_mtu_from_packet_size(uint16_t packet_size, uint8_t family);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_MTU_H
