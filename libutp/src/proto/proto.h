/*************************************************************************
    > File Name: proto.h
    > Author: eular
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
//   │                 Initial (01) [CHLO]                │
//   │───────────────────────────────────────────────────>│
//   │                                                    │
//   │                Handshake (02) [SHLO]               │
//   │<───────────────────────────────────────────────────│
//   │                                                    │
//   │                   Header [data]                    │
//   │───────────────────────────────────────────────────>│
//   │                                                    │

#define UDP_HEADER_SIZE         20 // UDP 头部长度

#define UTP_TYPE_NONE               0x00 // None
#define UTP_TYPE_INITIAL            0x01 // Client Hello
#define UTP_TYPE_HANDSHAKE          0x02 // Server Hello
#define UTP_TYPE_0RTT               0x03 // 0-RTT Data
#define UTP_TYPE_CONNECTION_CLOSE   0x04 // Connection Close

#define SESSION_TICKET_LIFETIME_S   7200 // 2 hours, max 18 hours
#define SESSION_TOKEN_SIZE          32   // token size

#define UTP_DEFAULT_ACK_THRESHOLD        2  // 每2个包发一次 ACK
#define UTP_DEFAULT_MAX_ACK_DELAY_MS     25 // 最大延迟 25ms
#define UTP_DEFAULT_REORDER_THRESHOLD    3  // 乱序超过3个包立即 ACK

namespace eular {
namespace utp {
#pragma pack(push, 1)
struct UTPHeaderProto {
    uint32_t    scid;           // source connection ID
    uint32_t    dcid;           // destination connection ID
    uint64_t    pn;             // packet number
    uint16_t    payload_length; // 有效载荷长度
    uint8_t     flags;          // 标志位
    uint8_t     reserve;        // 保留字段
};
#pragma pack(pop)
static_assert(sizeof(UTPHeaderProto) == 20, "UTPHeaderProto size error");

struct SessionTicket {
    uint64_t issue_time;              // 发放时间
    uint32_t lifetime_s;              // 有效期（秒）
    uint64_t server_connection_id;    // 服务端连接 ID

    // 缓存的连接参数
    uint64_t max_data;
    uint64_t max_stream_data;
    uint8_t  max_streams;

    // 应用层认证 token
    uint8_t  auth_token[SESSION_TOKEN_SIZE];
};

} // namespace utp
} // namespace eular

#endif // __PROTO_PROTO_H__
