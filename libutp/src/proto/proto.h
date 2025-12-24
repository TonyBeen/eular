/*************************************************************************
    > File Name: proto.h
    > Author: hsz
    > Brief:
    > Created Time: Tue 23 Dec 2025 05:19:55 PM CST
 ************************************************************************/

#ifndef __PROTO_PROTO_H__
#define __PROTO_PROTO_H__

#include <stdint.h>
#include <vector>

namespace eular {
namespace utp {

enum ProtoType {
    PROTO_TYPE_DATA         = 0x00,
    PROTO_TYPE_FIN          = 0x01,
    PROTO_TYPE_STATE        = 0x02,
    PROTO_TYPE_RESET        = 0x03,
    PROTO_TYPE_SYN          = 0x04,
    PROTO_TYPE_INVALID      = 0xFF,
};

struct HandshakeProto {
    uint8_t     type;
    uint8_t     reserved;
    uint16_t    version;
};


} // namespace utp
} // namespace eular

#endif // __PROTO_PROTO_H__
