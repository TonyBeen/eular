/*************************************************************************
    > File Name: stream_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 28 Jan 2026 05:33:06 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_STREAM_IMPL_H__
#define __UTP_CONTEXT_STREAM_IMPL_H__

#include <map>
#include <vector>

#include "proto/frame/stream.h"
#include "rbtree.h"
#include "util/ring_buffer.h"
#include "utp/stream.h"
#include "utp/types.h"

#define STREAM_TYPES 4

#define STREAM_ID_MASK 0b11

#define STREAM_ID_IS_CLIENT(ID)  ((ID & 1) == 0)
#define STREAM_ID_IS_SERVER(ID)  ((ID & 1) == 1)
#define STREAM_ID_IS_BI_DIR(ID)  ((ID & 2) == 0)
#define STREAM_ID_IS_UNI_DIR(ID) ((ID & 2) == 2)

namespace eular {
namespace utp {
class ConnectionImpl;
struct PacketIn;
class MemoryManager;

struct RecvFragment {
    struct rb_node treeNode;
    PacketIn*      packet{nullptr};
    const uint8_t* data{nullptr};
    size_t         len{0};
    size_t         consumed{0};
    uint64_t       offset{0};
    bool           fin{false};
    bool           accounted{false};
    size_t         memoryCost{0};

    size_t remaining() const { return len > consumed ? (len - consumed) : 0; }
};

class StreamImpl : public Stream
{
public:
    using SP = std::shared_ptr<StreamImpl>;

    static constexpr size_t kDefaultBufferCapacity = 64 * 1024;
    static constexpr size_t kMaxSendQueueBytes = 16 * 1024 * 1024;
    static constexpr size_t kMaxRecvFragmentBytes = 2 * 1024 * 1024;

    StreamImpl(ConnectionImpl* conn, uint32_t streamId, uint8_t priority = Stream::kPriorityDefault);
    ~StreamImpl();

    uint32_t id() const override;
    int32_t  write(const void* data, size_t len, bool fin) override;
    int32_t  read(void* buffer, size_t capacity) override;
    size_t   acquireWriteBuffer(MutableBufferView views[2], size_t maxBytes) override;
    int32_t  commitWrite(size_t bytes, bool fin) override;
    size_t   acquireReadViews(ConstBufferView views[2], size_t maxBytes) const override;
    int32_t  commitReadViews(size_t bytes) override;
    State    state() const override;
    bool     readable() const override;
    bool     writable() const override;
    void     close() override;
    int32_t  reset(uint16_t errorCode) override;
    bool     resetReceived() const override;
    int32_t  setPriority(uint8_t priority) override;
    uint8_t  priority() const override;
    void     setOnReadable(const OnReadable& cb) override;
    void     setOnWritable(const OnWritable& cb) override;
    void     setOnClosed(const OnClosed& cb) override;
    void     setOnReset(const OnReset& cb) override;

    // Feed an incoming STREAM frame payload into this stream.
    Status onFrame(const FrameStream& frame, PacketIn* packet = nullptr);
    Status onReset(uint16_t errorCode, bool fromPeer);

private:
    friend class ConnectionImpl;

private:
    Status        flushPendingSends(size_t maxBytes = static_cast<size_t>(-1));
    Status        onConnectionWritable(utp_time_t nowUs);
    void          onPacketAcked(uint64_t streamOffset, size_t len);
    size_t        appWriteCredit() const;
    uint64_t      sendBufferedEndOffset() const;
    bool          hasPendingSendWork() const;
    bool          shouldDeferSend(utp_time_t nowUs) const;
    utp_time_t    coalesceDelayRemainingUs(utp_time_t nowUs) const;
    Status        insertRecvFragment(RecvFragment* fragment, bool* inserted = nullptr);
    void          clearRecvFragments();
    RecvFragment* findLowerBound(uint64_t offset) const;
    RecvFragment* findPrev(uint64_t offset) const;
    RecvFragment* firstFragment() const;
    RecvFragment* nextFragment(const RecvFragment* fragment) const;
    void          eraseRecvFragment(RecvFragment* fragment);
    void          releaseRecvFragment(RecvFragment* fragment);
    bool          accountRecvFragment(RecvFragment* fragment, RecvFragment* replaced = nullptr);
    void          unaccountRecvFragment(RecvFragment* fragment);
    void          maybeAdvancePeerFin();
    size_t        contiguousReadableBytes(size_t maxBytes) const;
    void          maybeNotifyReadable(bool allowPeerFin = false);
    void          maybeNotifyClosed();
    void          maybeNotifyWritable(bool force = false);
    void          notifyResetOnce();

private:
    ConnectionImpl*                m_conn{nullptr};
    uint32_t                       m_streamId{0};
    uint64_t                       m_nextSendOffset{0};  // next unsent stream offset in sendBuffer
    uint64_t                       m_sendAckedOffset{0};
    uint64_t                       m_recvOffset{0};
    bool                           m_localFinQueued{false};
    bool                           m_localFinSent{false};
    bool                           m_peerFin{false};
    bool                           m_resetByPeer{false};
    bool                           m_resetNotified{false};
    bool                           m_closedNotified{false};
    uint16_t                       m_resetErrorCode{0};
    size_t                         m_sendQueuedBytes{0};
    size_t                         m_sendInFlightBytes{0};
    size_t                         m_recvBufferedBytes{0};
    size_t                         m_recvPinnedMemoryBytes{0};
    size_t                         m_recvFragmentCount{0};
    utp_time_t                     m_lastSendQueuedAtUs{0};  // enqueue timestamp for coalescing window(us)
    MemoryManager*                 m_recvMm{nullptr};
    std::unique_ptr<MemoryManager> m_ownedRecvMm;
    RingBuffer                     m_sendBuffer;
    struct rb_root                 m_recvFragmentsTree{RB_ROOT};
    OnReadable                     m_onReadable;
    OnWritable                     m_onWritable;
    OnClosed                       m_onClosed;
    OnReset                        m_onReset;
    bool                           m_notifyingReadable{false};
    bool                           m_notifyingWritable{false};
    std::map<uint64_t, uint64_t>   m_sendAckedRanges;

    /// @b Stream 调度属性
    uint8_t  m_priority{Stream::kPriorityDefault};  // stream 基础优先级(0最高,7最低)
    uint32_t m_schedWaitRounds{0};                  // STRICT 模式下累计等待轮次(用于 aging)
};

}  // namespace utp
}  // namespace eular

#endif  // __UTP_CONTEXT_STREAM_IMPL_H__
