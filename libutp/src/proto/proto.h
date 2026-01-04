/*************************************************************************
    > File Name: proto.h
    > Author: hsz
    > Brief:
    > Created Time: Tue 23 Dec 2025 05:19:55 PM CST
 ************************************************************************/

#ifndef __PROTO_PROTO_H__
#define __PROTO_PROTO_H__

#include <stdint.h>
#include <string.h>
#include <vector>
#include <string>
#include <array>

// Client                                              Server
// 
//   │  Initial (00) [CHLO]                               │
//   │───────────────────────────────────────────────────>│
//   │                                                    │
//   │  Initial (01) [SHLO] + Handshake (02) [FINISHED]   │
//   │<───────────────────────────────────────────────────│
//   │                                                    │
//   │  Handshake (02) [ACK]                              │
//   │───────────────────────────────────────────────────>│
//   │                                                    │
//   │  Short Header [data]                               │
//   │◄──────────────────────────────────────────────────►│
//

#define HEADER_FORM_LONG    1
#define HEADER_FORM_SHORT   0

// 标志位定义
#define UTP_FLAG_SYN    0b0001
#define UTP_FLAG_ACK    0b0010
#define UTP_FLAG_FIN    0b0100
#define UTP_FLAG_RST    0b1000
#define UTP_FLAG_ENC    0b10000  // 加密标志

#define SESSION_TICKET_LIFETIME_S   7200  // 2 hours, max 18 hours
#define SESSION_AUTH_TOKEN_SIZE     16

#define UTP_DEFAULT_ACK_THRESHOLD        2  // 每2个包发一次 ACK
#define UTP_DEFAULT_MAX_ACK_DELAY_MS     25 // 最大延迟 25ms
#define UTP_DEFAULT_REORDER_THRESHOLD    3  // 乱序超过3个包立即 ACK

namespace eular {
namespace utp {

enum UTPHeaderFlags {
    kFlagSyn    = UTP_FLAG_SYN,
    kFlagFin    = UTP_FLAG_FIN,
    kFlagAck    = UTP_FLAG_ACK,
    kFlagRst    = UTP_FLAG_RST,
    kFlagEnc    = UTP_FLAG_ENC,
};

struct UTPHeaderProto {
    uint32_t    scid;           // source connection ID
    uint32_t    dcid;           // destination connection ID
    uint64_t    pn;             // packet number
    uint16_t    payload_length; // 有效载荷长度
    uint8_t     flags;          // 标志位
    uint8_t     reserve;        // 保留字段
    uint32_t    crc32;          // 包头校验码
};
static_assert(sizeof(UTPHeaderProto) == 24, "UTPHeaderProto size error");

struct SessionTicket {
    uint64_t ticket_id;               // 票据 ID
    uint64_t issue_time;              // 发放时间
    uint32_t lifetime_s;              // 有效期（秒）
    uint64_t server_connection_id;    // 服务端连接 ID

    // 缓存的连接参数
    uint64_t max_data;
    uint64_t max_stream_data;
    uint8_t  max_streams;

    // 可选：应用层认证 token
    uint8_t  auth_token[16];
};

} // namespace utp
} // namespace eular

#endif // __PROTO_PROTO_H__
