#ifndef EULAR_UTP_CONTEXT_SEND_LEDGER_H
#define EULAR_UTP_CONTEXT_SEND_LEDGER_H

#include <stddef.h>
#include <stdint.h>

#include "proto/ack.h"
#include "proto/packet_out.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connection 持有的未确认包账本，运行期间不分配内存。
typedef struct utp_send_ledger {
    struct utp_packet_out_tailq unacked_packets;
    uint64_t                    bytes_in_flight;
    uint64_t                    retransmittable_bytes_in_flight;
    size_t                      packet_count;
    size_t                      retransmittable_packet_count;
    size_t                      packet_limit;
    uint32_t                    retransmittable_frame_mask;
} utp_send_ledger_t;

typedef struct utp_send_ledger_ack_result {
    uint64_t largest_acknowledged_packet_number;
    uint64_t largest_acknowledged_sent_time_us;
    uint64_t acknowledged_bytes;
    size_t   acknowledged_packet_count;
} utp_send_ledger_ack_result_t;

utp_internal_error_t utp_send_ledger_init(utp_send_ledger_t* ledger, size_t packet_limit,
                                          uint32_t retransmittable_frame_mask);
void                 utp_send_ledger_cleanup(utp_send_ledger_t* ledger);
utp_internal_error_t utp_send_ledger_track(utp_send_ledger_t* ledger, utp_packet_out_t* packet);
utp_internal_error_t utp_send_ledger_remove(utp_send_ledger_t* ledger, utp_packet_out_t* packet);
// 将已确认包移动到 acknowledged_packets，不分配内存，也不释放 PacketOut 所有权。
utp_internal_error_t utp_send_ledger_acknowledge(utp_send_ledger_t* ledger, const utp_ack_info_t* ack,
                                                 uint64_t                      largest_sent_packet_number,
                                                 struct utp_packet_out_tailq*  acknowledged_packets,
                                                 utp_send_ledger_ack_result_t* result);
utp_packet_out_t*    utp_send_ledger_find(const utp_send_ledger_t* ledger, uint64_t packet_number);
size_t               utp_send_ledger_packet_count(const utp_send_ledger_t* ledger);
uint64_t             utp_send_ledger_bytes_in_flight(const utp_send_ledger_t* ledger);
size_t               utp_send_ledger_retransmittable_packet_count(const utp_send_ledger_t* ledger);
uint64_t             utp_send_ledger_retransmittable_bytes_in_flight(const utp_send_ledger_t* ledger);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_SEND_LEDGER_H
