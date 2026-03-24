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

#include "utp/stream.h"
#include "proto/frame/stream.h"

#define STREAM_TYPES                4 

#define STREAM_ID_MASK              0b11

#define STREAM_ID_IS_CLIENT(ID)     ((ID & 1) == 0)
#define STREAM_ID_IS_SERVER(ID)     ((ID & 1) == 1)
#define STREAM_ID_IS_BI_DIR(ID)     ((ID & 2) == 0)
#define STREAM_ID_IS_UNI_DIR(ID)    ((ID & 2) == 2)

namespace eular {
namespace utp {
class ConnectionImpl;

class StreamImpl : public Stream {
public:
    using SP = std::shared_ptr<StreamImpl>;

    static constexpr size_t kDefaultBufferCapacity = 64 * 1024;
    static constexpr size_t kMaxSendQueueBytes = 4 * 1024 * 1024;
    static constexpr size_t kMaxRecvFragmentBytes = 4 * 1024 * 1024;

    StreamImpl(ConnectionImpl *conn, uint32_t streamId);
    ~StreamImpl();

    uint32_t id() const override;
    int32_t write(const void *data, size_t len, bool fin) override;
    int32_t read(void *buffer, size_t capacity) override;
    size_t acquireWriteBuffer(MutableBufferView views[2], size_t maxBytes) override;
    int32_t commitWrite(size_t bytes, bool fin) override;
    size_t acquireReadBuffer(ConstBufferView views[2], size_t maxBytes) const override;
    int32_t consumeRead(size_t bytes) override;
    State state() const override;
    bool readable() const override;
    bool writable() const override;
    void close() override;
    int32_t reset(uint16_t errorCode) override;
    bool resetReceived() const override;
    void setOnReadable(const OnReadable &cb) override;
    void setOnWritable(const OnWritable &cb) override;
    void setOnClosed(const OnClosed &cb) override;
    void setOnReset(const OnReset &cb) override;

    // Feed an incoming STREAM frame payload into this stream.
    int32_t onFrame(const FrameStream &frame);
    int32_t onReset(uint16_t errorCode, bool fromPeer);

private:
    friend class ConnectionImpl;

    class RingBuffer {
    public:
        RingBuffer() = default;
        explicit RingBuffer(size_t capacity);

        void ensureFree(size_t freeBytes);
        size_t size() const { return m_size; }
        size_t capacity() const { return m_buffer.size(); }
        size_t freeSize() const { return capacity() - m_size; }
        bool empty() const { return m_size == 0; }

        size_t readableViews(ConstBufferView views[2], size_t maxBytes) const;
        size_t writableViews(MutableBufferView views[2], size_t maxBytes);
        void produce(size_t bytes);
        void consume(size_t bytes);
        size_t write(const uint8_t *data, size_t len);
        size_t read(uint8_t *buffer, size_t len);

    private:
        std::vector<uint8_t> m_buffer;
        size_t m_head{0};
        size_t m_size{0};
    };

    struct PendingSendChunk {
        uint64_t offset{0};
        size_t bytes{0};
        bool fin{false};
    };

    struct RecvFragment {
        std::vector<uint8_t> data;
        bool fin{false};
    };

private:
    int32_t flushPendingSends();
    int32_t onConnectionWritable();
    size_t appWriteCredit() const;
    void drainRecvFragments();
    void maybeNotifyClosed();
    void maybeNotifyWritable(bool force = false);
    void notifyResetOnce();

private:
    ConnectionImpl *m_conn{nullptr};
    uint32_t m_streamId{0};
    uint64_t m_sendOffset{0};
    uint64_t m_sendBufferedOffset{0};
    uint64_t m_recvOffset{0};
    bool m_localFinQueued{false};
    bool m_localFinSent{false};
    bool m_peerFin{false};
    bool m_resetByPeer{false};
    bool m_resetNotified{false};
    bool m_closedNotified{false};
    uint16_t m_resetErrorCode{0};
    size_t m_sendQueuedBytes{0};
    size_t m_recvFragmentsBytes{0};
    RingBuffer m_sendBuffer;
    RingBuffer m_recvBuffer;
    std::vector<PendingSendChunk> m_sendQueue;
    std::map<uint64_t, RecvFragment> m_recvFragments;
    OnReadable m_onReadable;
    OnWritable m_onWritable;
    OnClosed m_onClosed;
    OnReset m_onReset;
    bool m_notifyingWritable{false};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_STREAM_IMPL_H__
