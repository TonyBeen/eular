#ifndef EULAR_UTP_CONTEXT_CONTEXT_H
#define EULAR_UTP_CONTEXT_CONTEXT_H

#include <utp/utp.h>

#include "connection/connection.h"
#include "context/event_loop.h"
#include "context/pending_incoming.h"
#include "proto/packet_in.h"
#include "socket/udp.h"
#include "util/log.h"

#define UTP_CONTEXT_MAX_CONNECTIONS          32u
#define UTP_CONTEXT_MAX_PENDING_INCOMING     32u
#define UTP_CONTEXT_PACKET_LIMIT             32u
#define UTP_CONTEXT_PACKET_IN_LIMIT          64u
#define UTP_CONTEXT_PACKET_IN_CAPACITY       65535u
#define UTP_CONTEXT_PENDING_PACKET_LIMIT     16u
#define UTP_CONTEXT_PENDING_STORAGE_CAPACITY 32768u

typedef struct utp_context_connection_slot {
    utp_connection_t connection;
    bool             used;
    bool             connected_reported;
    bool             closed_reported;
} utp_context_connection_slot_t;

typedef struct utp_context_pending_slot {
    utp_pending_incoming_t pending;
    uint8_t                storage[UTP_CONTEXT_PENDING_STORAGE_CAPACITY];
    bool                   used;
    bool                   queued;
} utp_context_pending_slot_t;

struct utp_context {
    utp_event_loop_t              event_loop;
    utp_event_t                   udp_event;
    utp_event_t                   timer_event;
    utp_udp_socket_t              udp_socket;
    utp_packet_in_pool_t          packet_in_pool;
    utp_context_connection_slot_t connections[UTP_CONTEXT_MAX_CONNECTIONS];
    utp_context_pending_slot_t    pending_incoming[UTP_CONTEXT_MAX_PENDING_INCOMING];
    uint32_t                      next_cid;
    utp_on_connected_fn           on_connected;
    void*                         on_connected_user_data;
    utp_on_connect_error_fn       on_connect_error;
    void*                         on_connect_error_user_data;
    utp_on_new_connection_fn      on_new_connection;
    void*                         on_new_connection_user_data;
    utp_on_connection_closed_fn   on_connection_closed;
    void*                         on_connection_closed_user_data;
    utp_logger_t                  logger;
    utp_log_tag_t                 tag;
};

#endif  // EULAR_UTP_CONTEXT_CONTEXT_H
