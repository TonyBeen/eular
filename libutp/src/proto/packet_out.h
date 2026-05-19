/*************************************************************************
    > File Name: packet_out.h
    > Author: eular
    > Brief:
    > Created Time: Fri 16 Jan 2026 03:59:10 PM CST
 ************************************************************************/

#ifndef __UTP_PROTO_PACKET_OUT_H__
#define __UTP_PROTO_PACKET_OUT_H__

#include <cstddef>
#include <queue.h>

#include "commom.h"
#include "utp/types.h"
#include "proto/frame.h"
#include "util/malo.hpp"
#include "proto/packet_common.h"
#include "congestion/bw_sampler.h"

namespace eular {
namespace utp {

enum PacketOutFlags : uint32_t {
    kPoHello        = (1 << 0), // 携带 initial 或 handshake 帧
    kPoEncrypted    = (1 << 1), // 包已加密
    kPoResetPackNo  = (1 << 2), // 包序号需要生成新值
    kPoNoEncrypt    = (1 << 3), // 包不加密
    kPoMtuProbe     = (1 << 4), // MTU 探测包
    kPoUnAcked      = (1 << 5), // 本包处于未确认队列
    kPoSched        = (1 << 6), // 本包处于调度队列, 等待发送
    kPoLost         = (1 << 7), // 本包处于丢包队列, 等待重传
    kPoLossRecorded = (1 << 8), // 已记录丢包事件, 等待重传
    kPoKeepPlaintext= (1 << 9), // 加密后仍保留 raw_data 明文，供重传改包号后再次加密
};

enum PacketOutLocalFlags : uint16_t {
    kPOLLoss        = (1 << 0), // 近期发生过丢包事件
    kPOLLimited     = (1 << 1), // 近期发生过丢包事件, 且当前拥塞受限
    kPOLFacked      = (1 << 2), // 被 FACK 检测出丢失
    kPOLTrackOnSend = (1 << 3), // 调度发送后需要进入 unacked 队列
};

enum FrameMetaFlags : uint8_t {
    kFMNone                 = 0,
    kFMRetransMustKeep      = (1 << 0), // 重传重组时必须保留该帧
    kFMTransientOnRetrans   = (1 << 1), // 重传时允许剔除(例如 piggyback ACK)
    kFMDroppableOnMtu       = (1 << 2), // MTU 收缩时可直接丢弃
    kFMSplittable           = (1 << 3), // 帧可拆分为多个包发送(当前仅 STREAM)
};

struct FrameMetaInfo {
    union {
        class StreamImpl*   stream; // 帧所属流, 含有流数据时才有效
        uintptr_t           data;
    } fmi_u;
    uint16_t            offset;     // 帧数据偏移
    uint16_t            length;     // 帧数据长度
    FrameType           frame_type; // 帧类型
    uint8_t             frame_flags;// FrameMetaFlags 位图
};

struct FrameMetaVec {
    TAILQ_ENTRY(FrameMetaInfo)  fv_next;   // 帧元信息队列指针
    struct FrameMetaInfo        frames[
        (64 - sizeof(decltype(fv_next))) / sizeof(FrameMetaInfo)
    ];
};
static constexpr size_t FRAME_META_VEC_SIZE = sizeof(FrameMetaVec);

struct PacketOutAttempt {
    utp_packno_t      packet_no;
    utp_time_t        sent_time;
    PacketOutAttempt *next;
};

struct PacketOutSlice {
    enum : uint8_t {
        kSourceRawOffset = 0,
        kSourceExternal = 1,
    };

    uint16_t offset;
    uint16_t length;
    const void *data;
    uint8_t source;

    const void *resolveData(const uint8_t *rawBase) const
    {
        if (source == kSourceExternal) {
            return data;
        }
        return rawBase + offset;
    }
};

static constexpr uint8_t PACKET_OUT_MAX_SLICES = 8;
static constexpr uint8_t PACKET_OUT_MAX_FRAMES = 8;

struct PacketOut {
    void                reset();
    void                initForReuse(uint8_t *raw, uint16_t alloc);
    bool                addSendAttempt(utp_packno_t packetNo, utp_time_t sentTime);
    void                clearSendAttempts();

    TAILQ_ENTRY(PacketOut)  po_next;    // 未确认包队列指针

    utp_time_t      sent_time;  // 发送时间戳(us)
    utp_packno_t    packno;     // 包序号
    utp_packno_t    ackno;      // 当前包含Ack时有效, 且表示最大已接收包序号
    PacketOut*      loss_chain; // 丢包链表指针, 循环链表

    uint32_t    frame_types;    // PacketFrameTypeBit 位图
    uint16_t    po_flags;       // PacketOutFlags 位图
    uint16_t    local_flags;    // PacketOutLocalFlags 位图

    uint16_t    data_size;          // 包数据大小
    uint16_t    encrypt_data_size;  // 加密后数据大小
    uint16_t    alloc_size;         // data 可用大小
    uint8_t     slice_count;        // 发送 slice 数量（基于 raw_data 偏移）
    uint8_t     frame_meta_count;   // 记录的逻辑帧数量
    uint32_t    stream_data_size;   // 本包携带的 STREAM 业务数据字节数(用于在途未确认流量预算)
    uint16_t    transient_ack_size; // piggyback ACK 字节数(重传时可剔除)
    uint32_t    stream_id;          // 本包关联的 stream id（仅携带 STREAM 数据时有效）
    uint64_t    stream_offset;      // 本包中 STREAM 数据起始偏移（仅携带 STREAM 数据时有效）

    uint8_t*        raw_data;       // 头部 + 多个帧数据缓存, 加密时需要预留16字节
    uint8_t*        encrypt_data;   // 加密后数据指针
    PacketOutSlice  slices[PACKET_OUT_MAX_SLICES];
    BWPacketState*  bw_state;       // 带宽采样状态指针
    // TODO(next): 为 MSG_ZEROCOPY 增加发送完成跟踪字段（cookie/完成状态/完成区间），
    // 并与 attempts/重传状态联合决定回收时机。
    PacketOutAttempt* attempts_head;
    PacketOutAttempt* attempts_tail;
    uint16_t          attempts_count;
    FrameMetaInfo     frame_meta[PACKET_OUT_MAX_FRAMES];

    union {
        FrameMetaInfo   one;
        FrameMetaVec    vec;
    } frames;
};
static constexpr size_t PACKET_OUT_SIZE = sizeof(PacketOut);

TAILQ_HEAD(PacketOutTailQ, PacketOut);

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_PACKET_OUT_H__
