#ifndef __NTRS_CLIENT_H__
#define __NTRS_CLIENT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTRS_MAX_IP_LEN     64
#define NTRS_MAX_TEXT_LEN   128
#define NTRS_MAX_CANDIDATES 8

typedef uint16_t ntrs_nat_class_t;
typedef uint16_t ntrs_nat_flags_t;
typedef uint16_t ntrs_mapping_behavior_t;
typedef uint16_t ntrs_filtering_behavior_t;
typedef uint8_t  ntrs_punch_order_t;
typedef uint8_t  ntrs_connect_role_t;

enum {
    NTRS_NAT_CLASS_UNKNOWN = 0,
    NTRS_NAT_CLASS_OPEN_PUBLIC = 1,
    NTRS_NAT_CLASS_FULL_CONE = 2,
    NTRS_NAT_CLASS_IP_RESTRICTED = 3,
    NTRS_NAT_CLASS_PORT_RESTRICTED = 4,
    NTRS_NAT_CLASS_SYMMETRIC = 5,
};

enum {
    NTRS_NAT_FLAG_NONE = 0,
    NTRS_NAT_FLAG_LOCAL_ADDR_PUBLIC = 1u << 0,
    NTRS_NAT_FLAG_UDP_BLOCKED = 1u << 1,
    NTRS_NAT_FLAG_PROBE_DEGRADED = 1u << 2,
    NTRS_NAT_FLAG_MAPPING_UNSTABLE = 1u << 3,
    NTRS_NAT_FLAG_MULTI_EXTERNAL_IP = 1u << 4,
};

enum {
    NTRS_MAPPING_UNKNOWN = 0,
    NTRS_MAPPING_ENDPOINT_INDEPENDENT = 1,
    NTRS_MAPPING_ADDRESS_DEPENDENT = 2,
    NTRS_MAPPING_ADDRESS_AND_PORT_DEPENDENT = 3,
    NTRS_MAPPING_UNSTABLE = 4,
};

enum {
    NTRS_FILTERING_UNKNOWN = 0,
    NTRS_FILTERING_ENDPOINT_INDEPENDENT = 1,
    NTRS_FILTERING_ADDRESS_DEPENDENT = 2,
    NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT = 3,
    NTRS_FILTERING_BLOCKED = 4,
};

enum {
    NTRS_PUNCH_ORDER_UNKNOWN = 0,
    NTRS_PUNCH_ORDER_SEND_FIRST = 1,
    NTRS_PUNCH_ORDER_WAIT_FIRST = 2,
    NTRS_PUNCH_ORDER_SIMULTANEOUS = 3,
};

enum {
    NTRS_CONNECT_ROLE_UNKNOWN = 0,
    NTRS_CONNECT_ROLE_INITIATOR = 1,
    NTRS_CONNECT_ROLE_LISTENER = 2,
};

typedef struct ntrs_nat_info {
    char                      local_ip[NTRS_MAX_IP_LEN];
    uint16_t                  local_port;
    char                      srflx_ip[NTRS_MAX_IP_LEN];
    uint16_t                  srflx_port;
    char                      srflx_ip_2[NTRS_MAX_IP_LEN];
    uint16_t                  srflx_port_2;
    bool                      mapping_stable;
    char                      nat_risk[NTRS_MAX_TEXT_LEN];
    bool                      probe1_ok;
    bool                      probe2_ok;
    int32_t                   probe1_rtt_ms;
    int32_t                   probe2_rtt_ms;
    int32_t                   probe_rounds;
    int32_t                   probe1_success_count;
    int32_t                   probe2_success_count;
    int32_t                   probe1_distinct_mappings;
    int32_t                   probe2_distinct_mappings;
    ntrs_nat_class_t          nat_class;
    ntrs_nat_flags_t          nat_flags;
    ntrs_mapping_behavior_t   mapping_behavior;
    ntrs_filtering_behavior_t filtering_behavior;
    char                      nat_type[NTRS_MAX_TEXT_LEN];
    bool                      filter_same_ip_diff_port_rx;
    bool                      filter_diff_ip_rx;
} ntrs_nat_info_t;

typedef struct ntrs_peer_candidate {
    char     ip[NTRS_MAX_IP_LEN];
    uint16_t port;
    char     type[NTRS_MAX_TEXT_LEN];
} ntrs_peer_candidate_t;

typedef struct ntrs_session_signal {
    char                      peer_id[NTRS_MAX_TEXT_LEN];
    ntrs_nat_class_t          peer_nat_class;
    ntrs_nat_flags_t          peer_nat_flags;
    ntrs_mapping_behavior_t   peer_mapping_behavior;
    ntrs_filtering_behavior_t peer_filtering_behavior;
    char                      peer_nat_type[NTRS_MAX_TEXT_LEN];
    char                      session_id[NTRS_MAX_TEXT_LEN];
    char                      peer_session_token[NTRS_MAX_TEXT_LEN];
    ntrs_punch_order_t        punch_order;
    ntrs_connect_role_t       connect_role;
    uint32_t                  warmup_rounds;
    uint32_t                  warmup_interval_ms;
    uint32_t                  expire_at;
    uint32_t                  candidate_count;
    ntrs_peer_candidate_t     candidates[NTRS_MAX_CANDIDATES];
} ntrs_session_signal_t;

typedef struct ntrs_detect_nat_options {
    int32_t probe_rounds;
    int32_t retries_per_round;
    bool    enable_filter_probe;
    bool    verbose;
} ntrs_detect_nat_options_t;

typedef struct ntrs_async_client   ntrs_async_client_t;
struct event_base;

typedef enum ntrs_async_request_type {
    NTRS_ASYNC_AUTH = 1,
    NTRS_ASYNC_REQUEST_PROBE_ENDPOINTS = 2,
    NTRS_ASYNC_DETECT_NAT = 3,
    NTRS_ASYNC_REGISTER_PEER = 4,
    NTRS_ASYNC_UNREGISTER_PEER = 5,
    NTRS_ASYNC_CREATE_SESSION = 6,
    NTRS_ASYNC_WAIT_FOR_SIGNAL = 7,
    NTRS_ASYNC_UDP_HOLE_PUNCH = 8,
    NTRS_ASYNC_CONNECT_CONTROL = 9,
    NTRS_ASYNC_HEARTBEAT = 10,
} ntrs_async_request_type_t;

typedef struct ntrs_async_result {
    uint64_t                  request_id;
    ntrs_async_request_type_t type;
    bool                      success;
    bool                      cancelled;
    char                      error_message[NTRS_MAX_TEXT_LEN];
    char                      session_token[NTRS_MAX_TEXT_LEN];
    uint32_t                  lease_default_sec;
    char                      stun1[NTRS_MAX_TEXT_LEN];
    char                      stun2[NTRS_MAX_TEXT_LEN];
    ntrs_nat_info_t           nat_info;
    ntrs_session_signal_t     session_signal;
    ntrs_peer_candidate_t     selected_candidate;
    int32_t                   control_fd;
} ntrs_async_result_t;

typedef void (*ntrs_async_callback_t)(const ntrs_async_result_t* result, void* user_data);

void ntrs_nat_info_init(ntrs_nat_info_t* info);
void ntrs_detect_nat_options_init(ntrs_detect_nat_options_t* options);

int32_t ntrs_connect_control(const char* ntrs_ip, uint16_t ntrs_port);

bool ntrs_request_probe_endpoints(int32_t control_fd, const char* session_token, char* stun1, size_t stun1_len,
                                  char* stun2, size_t stun2_len);

bool ntrs_auth(int32_t control_fd, const char* peer_id, const char* bootstrap_token, char* session_token,
               size_t session_token_len, uint32_t* lease_default_sec);

bool ntrs_register_peer(int32_t control_fd, const char* peer_id, const char* device_id, const char* session_token,
                        const ntrs_nat_info_t* nat);

bool ntrs_unregister_peer(int32_t control_fd, const char* peer_id, const char* session_token, const char* reason);

bool ntrs_create_session(int32_t control_fd, const char* src_peer_id, const char* dst_peer_id,
                         const char* session_token, ntrs_session_signal_t* out);

bool ntrs_wait_for_signal(int32_t control_fd, int32_t timeout_ms, ntrs_session_signal_t* out);

bool ntrs_try_udp_hole_punch(int32_t udp_sock, const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                             int32_t send_rounds, int32_t select_wait_ms, ntrs_peer_candidate_t* selected);

ntrs_async_client_t* ntrs_async_client_create(struct event_base* base);
void                 ntrs_async_client_destroy(ntrs_async_client_t* client);
bool                 ntrs_async_client_cancel(ntrs_async_client_t* client, uint64_t request_id);

bool ntrs_async_auth(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd, const char* peer_id,
                     const char* bootstrap_token, ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_connect_control(ntrs_async_client_t* client, uint64_t* request_id, const char* host, uint16_t port,
                                int32_t timeout_ms, ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_request_probe_endpoints(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                        const char* session_token, ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_detect_nat(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock, const char* stun1_host,
                           uint16_t stun1_port, const char* stun2_host, uint16_t stun2_port, int32_t control_fd,
                           const char* session_token, const ntrs_detect_nat_options_t* options,
                           ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_register_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                              const char* peer_id, const char* device_id, const char* session_token,
                              const ntrs_nat_info_t* nat, ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_unregister_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                const char* peer_id, const char* session_token, const char* reason,
                                ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_heartbeat(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd, const char* peer_id,
                          const char* session_token, uint32_t lease_seq, ntrs_async_callback_t callback,
                          void* user_data);

bool ntrs_async_create_session(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                               const char* src_peer_id, const char* dst_peer_id, const char* session_token,
                               ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_wait_for_signal(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                int32_t timeout_ms, ntrs_async_callback_t callback, void* user_data);

bool ntrs_async_try_udp_hole_punch(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock,
                                   const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                                   int32_t send_rounds, int32_t interval_ms, ntrs_async_callback_t callback,
                                   void* user_data);

#ifdef __cplusplus
}
#endif

#endif
