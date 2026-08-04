#ifndef EULAR_UTP_CONTEXT_PENDING_INCOMING_H
#define EULAR_UTP_CONTEXT_PENDING_INCOMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/frame.h"
#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum utp_pending_incoming_result {
    UTP_PENDING_INCOMING_BUFFERED = 0,
    UTP_PENDING_INCOMING_PROMOTE
} utp_pending_incoming_result_t;

typedef utp_internal_error_t (*utp_pending_incoming_replay_fn)(const uint8_t* packet, size_t packet_length,
                                                               void* user_data);

// storage 由 Context 持有并在创建 pending 项时传入。每个缓存包由两字节长度前缀和完整线数据组成，
// 因此接收热路径不需要动态分配。
typedef struct utp_pending_incoming {
    utp_address_t peer;
    uint8_t*      storage;
    size_t        storage_capacity;
    size_t        storage_length;
    size_t        packet_limit;
    size_t        packet_count;
    uint32_t      local_cid;
    uint32_t      peer_cid;
    uint64_t      last_handshake_packet_number;
    uint64_t      handshake_retransmission_deadline_us;
    uint32_t      handshake_retransmission_count;
    bool          accepted;
    bool          handshake_sent;
} utp_pending_incoming_t;

utp_internal_error_t utp_pending_incoming_init(utp_pending_incoming_t* pending, uint32_t local_cid, uint32_t peer_cid,
                                               const utp_address_t* peer, uint8_t* storage, size_t storage_capacity,
                                               size_t packet_limit);
void                 utp_pending_incoming_reset(utp_pending_incoming_t* pending);
utp_internal_error_t utp_pending_incoming_accept(utp_pending_incoming_t* pending);
utp_internal_error_t utp_pending_incoming_mark_handshake_sent(utp_pending_incoming_t* pending,
                                                              uint64_t handshake_packet_number, uint64_t now_us);
uint64_t             utp_pending_incoming_handshake_deadline(const utp_pending_incoming_t* pending);
utp_internal_error_t utp_pending_incoming_on_packet(utp_pending_incoming_t* pending, const uint8_t* packet,
                                                    size_t packet_length, const utp_address_t* peer,
                                                    utp_pending_incoming_result_t* result);
utp_internal_error_t utp_pending_incoming_replay(const utp_pending_incoming_t*  pending,
                                                 utp_pending_incoming_replay_fn replay, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_PENDING_INCOMING_H
