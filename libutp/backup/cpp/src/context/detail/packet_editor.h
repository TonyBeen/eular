/*************************************************************************
    > File Name: packet_editor.h
    > Author: eular
    > Brief:
 ************************************************************************/

#ifndef __UTP_CONTEXT_DETAIL_PACKET_EDITOR_H__
#define __UTP_CONTEXT_DETAIL_PACKET_EDITOR_H__

#include <cstdint>

namespace eular {
namespace utp {

class ConnectionImpl;
struct PacketOut;

namespace detail {

class PacketEditor
{
public:
    static bool RewritePacketNumber(ConnectionImpl *conn, PacketOut *pkt);
    static bool RewritePayloadLength(PacketOut *pkt, uint16_t payloadLen);
    static bool StripTransientAckPayload(PacketOut *pkt);
};

} // namespace detail
} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_DETAIL_PACKET_EDITOR_H__
