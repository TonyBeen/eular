/*************************************************************************
    > File Name: packet_out.h
    > Author: eular
    > Brief:
    > Created Time: Fri 16 Jan 2026 03:59:10 PM CST
 ************************************************************************/

#ifndef __UTP_PROTO_PACKET_OUT_H__
#define __UTP_PROTO_PACKET_OUT_H__

#include "commom.h"
#include "proto/frame.h"
#include "proto/packet_common.h"
#include "congestion/bw_sampler.h"
#include "util/malo.hpp"

namespace eular {
namespace utp {

enum PacketOutFlags {
    kPoHello        = (1 << 0), // 携带 initial 或 handshake 帧
    kPoEncrypted    = (1 << 1), // 包已加密
    kPoResetPackNo  = (1 << 2), // 包序号需要生成新值
    kPoNoEncrypt    = (1 << 3), // 包不加密
    kPoMtuProbe     = (1 << 4), // MTU 探测包
    kPoUnAcked      = (1 << 5), // 本包处于未确认队列
    kPoSched        = (1 << 6), // 本包处于调度队列, 等待发送
    kPoLost         = (1 << 7), // 本包处于丢包队列, 等待重传
};

struct FrameMetaInfo {
    union {
        class StreamImpl*   stream; // 帧所属流, 含有流数据时才有效
        uintptr_t           data;
    } fmi_u;
    #define stream      fmi_u.stream
    uint16_t            offset;     // 帧数据偏移
    uint16_t            length;     // 帧数据长度
    FrameType           frame_type; // 帧类型
};

struct FrameMetaVec {
    TAILQ_ENTRY(FrameMetaInfo)  fv_next;   // 帧元信息队列指针
    struct FrameMetaInfo        frames[
        (64 - sizeof(decltype(fv_next))) / sizeof(FrameMetaInfo)
    ];
};
static constexpr size_t FRAME_META_VEC_SIZE = sizeof(FrameMetaVec);

struct PacketOut {
    TAILQ_ENTRY(PacketOut)  po_next;    // 未确认包队列指针

    uint64_t    sent_time;  // 发送时间戳(us)
    uint64_t    packno;     // 包序号
    uint64_t    ackno;      // 当前包含Ack时有效, 且表示最大已接收包序号
    PacketOut*  loss_chain; // 丢包链表指针, 循环链表

    PacketFrameTypeBit  frame_types;    // 包含的帧类型位图
    PacketOutFlags      po_flags;       // PacketOutFlags 位图

    uint16_t    data_size;  // 包数据大小
    uint16_t    encrypt_data_size; // 加密后数据大小
    uint16_t    alloc_size; // data 可用大小

    uint8_t*        raw_data;       // 多个帧数数据缓存
    uint8_t*        encrypt_data;   // 加密后数据指针
    BWPacketState*  bw_state;       // 带宽采样状态指针

    union {
        FrameMetaInfo   one;
        FrameMetaVec    vec;
    } frames;

    static PacketOut* Create();
};
static constexpr size_t PACKET_OUT_SIZE = sizeof(PacketOut);

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_PACKET_OUT_H__
