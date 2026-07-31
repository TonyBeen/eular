#ifndef EULAR_UTP_CONNECTION_CONNECTION_H
#define EULAR_UTP_CONNECTION_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "connection/stream.h"
#include "context/ack_scheduler.h"
#include "context/send_control.h"
#include "proto/frame.h"
#include "proto/packet_in.h"
#include "socket/address.h"
#include "util/receive_history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_CONNECTION_MAX_RECEIVE_RANGES             32u
#define UTP_CONNECTION_MAX_STREAMS                    8u
#define UTP_CONNECTION_CONTROL_SLOT_COUNT             (2u + 3u * UTP_CONNECTION_MAX_STREAMS)
#define UTP_CONNECTION_RECV_REASSEMBLY_MEMORY_LIMIT   (16u * 1024u * 1024u)
#define UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT 4096u

typedef enum utp_connection_role { UTP_CONNECTION_ROLE_ACTIVE = 0, UTP_CONNECTION_ROLE_PASSIVE } utp_connection_role_t;

typedef enum utp_connection_state {
    UTP_CONNECTION_STATE_NEW = 0,
    UTP_CONNECTION_STATE_INITIAL_SENT,
    UTP_CONNECTION_STATE_CONNECTED,
    UTP_CONNECTION_STATE_CLOSING,
    UTP_CONNECTION_STATE_DRAINING,
    UTP_CONNECTION_STATE_CLOSED
} utp_connection_state_t;

typedef struct utp_connection_control_slot {
    uint64_t value;
    uint64_t final_size;
    uint32_t stream_id;
    uint32_t generation;
    uint32_t in_flight_generation;
    uint16_t error_code;
    uint8_t  frame_type;
    bool     pending;
    bool     queued;
    bool     in_flight;
} utp_connection_control_slot_t;

// This is a private, connection-owned transport state. Context owns CID lookup and UDP I/O.
typedef struct utp_connection {
    utp_send_control_t            send_control;
    utp_receive_history_t         receive_history;
    utp_ack_scheduler_t           ack_scheduler;
    utp_packet_out_pool_t         packet_pool;
    utp_stream_t                  streams[UTP_CONNECTION_MAX_STREAMS];
    utp_connection_control_slot_t control_slots[UTP_CONNECTION_CONTROL_SLOT_COUNT];
    utp_address_t                 peer;
    uint32_t                      local_cid;
    uint32_t                      peer_cid;
    uint32_t                      next_stream_id[UTP_STREAM_TYPES];
    uint32_t                      peer_max_stream_data_ids[UTP_CONNECTION_MAX_STREAMS];
    uint64_t                      peer_max_stream_data_values[UTP_CONNECTION_MAX_STREAMS];
    uint64_t                      peer_max_data;
    uint64_t                      local_max_data_advertised;
    uint64_t                      stream_data_sent_total;
    uint64_t                      local_stream_data_received_total;
    uint64_t                      local_stream_data_consumed_total;
    uint64_t                      last_max_data_sent_us;
    uint64_t                      last_data_blocked_sent_us;
    uint16_t                      packet_capacity;
    size_t                        recv_reassembly_memory_bytes;
    size_t                        recv_reassembly_fragment_count;
    uint64_t                      rx_bytes;
    uint64_t                      tx_bytes;
    uint64_t                      peer_handshake_packet_number;
    uint64_t                      retransmission_deadline_us;
    uint64_t                      close_deadline_us;
    uint64_t                      close_last_sent_us;
    uint64_t                      close_pto_us;
    uint16_t                      close_error_code;
    uint8_t                       stream_scheduler_mode;
    uint8_t                       stream_scheduler_cursor;
    bool                          close_pending;
    utp_connection_role_t         role;
    size_t                        peer_max_stream_data_count;
    utp_connection_state_t        state;
} utp_connection_t;

utp_internal_error_t utp_connection_init(utp_connection_t* connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t* peer, size_t packet_limit,
                                         uint16_t packet_capacity);
void                 utp_connection_cleanup(utp_connection_t* connection);

// Builds a complete plaintext packet and places it in the bounded send queue.
utp_internal_error_t utp_connection_queue_packet(utp_connection_t* connection, uint8_t packet_type,
                                                 const uint8_t* payload, size_t payload_length, bool track_on_send);
utp_internal_error_t utp_connection_queue_close(utp_connection_t* connection, uint16_t error_code);
// Returns a scheduled packet or a retransmission with a fresh packet number.
utp_packet_out_t* utp_connection_next_packet_to_send(utp_connection_t* connection);
utp_packet_out_t* utp_connection_next_packet_to_send_at(utp_connection_t* connection, uint64_t now_us);
// Marks a packet as successfully written. Non-tracked packets are returned to the pool here.
utp_internal_error_t utp_connection_on_packet_sent(utp_connection_t* connection, utp_packet_out_t* packet,
                                                   uint64_t now_us);
// Releases a packet that was never written to UDP and restores its pending transport state.
void utp_connection_on_packet_abandoned(utp_connection_t* connection, const utp_packet_out_t* packet);
// Validates peer/CIDs, processes ACK and lifecycle frames, and records received packet numbers.
utp_internal_error_t utp_connection_on_packet_received(utp_connection_t* connection, const uint8_t* packet,
                                                       size_t packet_length, const utp_address_t* peer,
                                                       uint64_t now_us);
utp_internal_error_t utp_connection_on_packet_in_received(utp_connection_t* connection, utp_packet_in_t* packet,
                                                          const utp_address_t* peer, uint64_t now_us);
utp_internal_error_t utp_connection_queue_ack(utp_connection_t* connection, uint64_t now_us);
uint32_t             utp_connection_ack_pending_count(const utp_connection_t* connection);
uint64_t             utp_connection_ack_deadline(const utp_connection_t* connection);
utp_internal_error_t utp_connection_ensure_retransmission_deadline(utp_connection_t* connection, uint64_t now_us);
utp_internal_error_t utp_connection_on_retransmission_timeout(utp_connection_t* connection, uint64_t now_us);
uint64_t             utp_connection_retransmission_deadline(const utp_connection_t* connection);
uint64_t             utp_connection_close_deadline(const utp_connection_t* connection);
utp_internal_error_t utp_connection_set_stream_scheduler_mode(utp_connection_t* connection, uint8_t mode);
utp_internal_error_t utp_connection_create_stream(utp_connection_t* connection, bool bidirectional,
                                                  uint32_t* out_stream_id);
utp_stream_t*        utp_connection_find_stream(utp_connection_t* connection, uint32_t stream_id);
utp_internal_error_t utp_connection_stream_write(utp_connection_t* connection, uint32_t stream_id, const uint8_t* data,
                                                 size_t length, bool fin);
utp_internal_error_t utp_connection_stream_reset(utp_connection_t* connection, uint32_t stream_id, uint16_t error_code);
utp_internal_error_t utp_connection_stream_read(utp_connection_t* connection, uint32_t stream_id, uint8_t* buffer,
                                                size_t capacity, size_t* out_length, bool* out_fin);
utp_internal_error_t utp_connection_stream_acquire_read_view(utp_connection_t* connection, uint32_t stream_id,
                                                             utp_stream_read_view_t* out_view);
utp_internal_error_t utp_connection_stream_commit_read_view(utp_connection_t* connection, uint32_t stream_id,
                                                            uint64_t offset, size_t length);

utp_connection_state_t utp_connection_state(const utp_connection_t* connection);
bool                   utp_connection_is_connected(const utp_connection_t* connection);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONNECTION_CONNECTION_H
