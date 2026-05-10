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

#include <array>
#include <string>
#include <vector>

#define UTP_HEADER_SIZE 20  // UDP 头部长度

#define UTP_TYPE_NONE             0x00  // None
#define UTP_TYPE_INITIAL          0x01  // Client Hello
#define UTP_TYPE_HANDSHAKE        0x02  // Server Hello
#define UTP_TYPE_0RTT             0x03  // 0-RTT Data
#define UTP_TYPE_CONNECTION_CLOSE 0x04  // Connection Close
#define UTP_TYPE_CTRL             0x05  // Control Frame

#define UTP_DEFAULT_ACK_THRESHOLD     5   // 每5个包发一次 ACK
#define UTP_DEFAULT_MAX_ACK_DELAY_MS  25  // 最大延迟 25ms
#define UTP_DEFAULT_REORDER_THRESHOLD 3   // 乱序超过3个包立即 ACK
#define UTP_PROTOCOL_VERSION          2

namespace eular {
namespace utp {
struct UTPHeaderProto {
    uint32_t scid;            // source connection ID
    uint32_t dcid;            // destination connection ID
    uint64_t pn;              // packet number
    uint16_t payload_length;  // 有效载荷长度
    uint8_t types;            // 类型
    uint8_t reserve;          // 保留字段
};

}  // namespace utp
}  // namespace eular

#endif  // __PROTO_PROTO_H__
