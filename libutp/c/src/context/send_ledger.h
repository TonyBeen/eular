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
    struct utp_packet_out_tailq unacked_packets;                  // 按发送顺序的未确认包队列
    uint64_t                    bytes_in_flight;                  // 所有飞行中包字节数
    uint64_t                    retransmittable_bytes_in_flight;  // 可重传飞行字节数
    size_t                      packet_count;                     // 未确认包数
    size_t                      retransmittable_packet_count;     // 可重传未确认包数
    size_t                      packet_limit;                     // 未确认包数量上限
    uint32_t                    retransmittable_frame_mask;       // 可重传帧类型位图
} utp_send_ledger_t;

typedef struct utp_send_ledger_ack_result {
    uint64_t largest_acknowledged_packet_number;  // 本批 ACK 最大确认包号
    uint64_t largest_acknowledged_sent_time_us;   // 对应包的发送时刻
    uint64_t acknowledged_bytes;                  // 本批新确认字节数
    size_t   acknowledged_packet_count;           // 本批新确认包数
} utp_send_ledger_ack_result_t;

utp_internal_error_t utp_send_ledger_init(utp_send_ledger_t* ledger, size_t packet_limit,
                                          uint32_t retransmittable_frame_mask);
void                 utp_send_ledger_cleanup(utp_send_ledger_t* ledger);
utp_internal_error_t utp_send_ledger_track(utp_send_ledger_t* ledger, utp_packet_out_t* packet);
utp_internal_error_t utp_send_ledger_remove(utp_send_ledger_t* ledger, utp_packet_out_t* packet);
/** @brief 严格校验 ACK 区间不超出已发送包号。 */
utp_internal_error_t utp_send_ledger_validate_ack(const utp_ack_info_t* ack, uint64_t largest_sent_packet_number);
utp_packet_out_t*    utp_send_ledger_find(const utp_send_ledger_t* ledger, uint64_t packet_number);
size_t               utp_send_ledger_packet_count(const utp_send_ledger_t* ledger);
uint64_t             utp_send_ledger_bytes_in_flight(const utp_send_ledger_t* ledger);
size_t               utp_send_ledger_retransmittable_packet_count(const utp_send_ledger_t* ledger);
uint64_t             utp_send_ledger_retransmittable_bytes_in_flight(const utp_send_ledger_t* ledger);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_SEND_LEDGER_H
