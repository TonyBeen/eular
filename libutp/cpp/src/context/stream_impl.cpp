/*************************************************************************
    > File Name: stream_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 28 Jan 2026 05:33:08 PM CST
 ************************************************************************/

#include "context/stream_impl.h"

#include <algorithm>
#include <cstring>

#include <limits>

#include "utp/errno.h"
#include "context/connection_impl.h"
#include "logger/logger.h"
#include "proto/packet_in.h"
#include "util/error.h"
#include "util/mm.h"
#include "util/time.h"

namespace eular {
namespace utp {

static inline int32_t StreamErr(utp_error_t err, Status &st)
{
    if (err == UTP_ERR_OK) {
        st = Status::OK();
        return 0;
    }

    st = Status::Error(err, fmt::format("stream operation failed: {}", static_cast<uint32_t>(err)));
    SetLastErrorV(st.code(), st.message());
    return -1;
}

constexpr size_t kMinSendBudgetBytesPerWritable = 4 * 1024;
constexpr size_t kMaxSendBudgetBytesPerWritable = 64 * 1024;
constexpr size_t kDefaultMtuPacketsPerWritable  = 4;

static size_t StreamPayloadMtuBudget(const ConnectionImpl *conn)
{
    if (conn == nullptr) {
        return 1;
    }
    return std::max<size_t>(conn->streamPayloadBudgetHint(), 1);
}

static size_t StreamWritableSendBudget(const ConnectionImpl *conn)
{
    // Budget one writable round by MTU-sized payload quanta, then clamp to
    // avoid tiny rounds and to keep per-stream fairness under bursty loads.
    const size_t mtuPayload = StreamPayloadMtuBudget(conn);
    const size_t mtuBasedBudget = mtuPayload * kDefaultMtuPacketsPerWritable;
    return (std::max)(kMinSendBudgetBytesPerWritable,
                      (std::min)(mtuBasedBudget, kMaxSendBudgetBytesPerWritable));
}

static size_t StreamSendBufferLimit(const ConnectionImpl *conn)
{
    if (conn == nullptr || conn->config() == nullptr || conn->config()->stream_send_buffer_limit == 0) {
        return StreamImpl::kDefaultBufferCapacity;
    }
    return std::max<size_t>(conn->config()->stream_send_buffer_limit, 1);
}

StreamImpl::StreamImpl(ConnectionImpl *conn, uint32_t streamId, uint8_t priority) :
    m_conn(conn),
    m_streamId(streamId),
    m_priority(std::min<uint8_t>(priority, Stream::kPriorityLowest)),
    m_sendBuffer(StreamSendBufferLimit(conn))
{
    assert(m_conn != nullptr);
    m_recvMm = &m_conn->m_mm;
}

StreamImpl::~StreamImpl()
{
    clearRecvFragments();
}

uint32_t StreamImpl::id() const
{
    return m_streamId;
}

int32_t StreamImpl::write(const void *data, size_t len, bool fin)
{
    Status st;
    if (len == 0 && !fin) {
        return StreamErr(UTP_ERR_INVALID_PARAM, st);
    }
    if (len > 0 && data == nullptr) {
        return StreamErr(UTP_ERR_INVALID_PARAM, st);
    }

    if (m_localFinQueued) {
        return StreamErr(UTP_ERR_STREAM_CLOSED, st);
    }

    if (len > appWriteCredit()) {
        return StreamErr(UTP_ERR_WOULD_BLOCK, st);
    }

    MutableBufferView views[2];
    size_t acquired = acquireWriteBuffer(views, len);
    if (acquired < len) {
        return StreamErr(UTP_ERR_WOULD_BLOCK, st);
    }

    const uint8_t *payload = static_cast<const uint8_t *>(data);
    size_t copied = 0;
    for (size_t i = 0; i < 2 && copied < len; ++i) {
        if (views[i].data == nullptr || views[i].len == 0) {
            continue;
        }
        const size_t n = std::min(views[i].len, len - copied);
        std::memcpy(views[i].data, payload + copied, n);
        copied += n;
    }

    return commitWrite(len, fin);
}

int32_t StreamImpl::read(void *buffer, size_t capacity)
{
    Status st;
    if (buffer == nullptr || capacity == 0) {
        return StreamErr(UTP_ERR_INVALID_PARAM, st);
    }

    const size_t contiguous = contiguousReadableBytes(capacity);
    if (contiguous == 0) {
        maybeAdvancePeerFin();
        return m_peerFin ? 0 : StreamErr(UTP_ERR_WOULD_BLOCK, st);
    }

    uint8_t *out = static_cast<uint8_t *>(buffer);
    size_t copied = 0;
    RecvFragment *fragment = firstFragment();
    while (fragment != nullptr && copied < contiguous) {
        const uint64_t logicalOffset = fragment->offset + fragment->consumed;
        if (logicalOffset != m_recvOffset) {
            break;
        }

        const size_t remaining = fragment->remaining();
        if (remaining == 0) {
            if (fragment->fin) {
                m_peerFin = true;
            }
            eraseRecvFragment(fragment);
            fragment = firstFragment();
            continue;
        }

        const size_t n = std::min(remaining, contiguous - copied);
        std::memcpy(out + copied, fragment->data + fragment->consumed, n);
        fragment->consumed += n;
        m_recvBufferedBytes = (m_recvBufferedBytes >= n) ? (m_recvBufferedBytes - n) : 0;
        m_recvOffset += n;
        copied += n;

        if (fragment->remaining() == 0) {
            if (fragment->fin) {
                m_peerFin = true;
            }
            eraseRecvFragment(fragment);
        }
        fragment = firstFragment();
    }

    maybeAdvancePeerFin();
    if (copied > 0 && m_conn != nullptr) {
        m_conn->onStreamBytesConsumed(m_streamId, copied);
    }
    maybeNotifyClosed();
    return static_cast<int32_t>(copied);
}

size_t StreamImpl::acquireWriteBuffer(MutableBufferView views[2], size_t maxBytes)
{
    if (m_localFinQueued || views == nullptr || maxBytes == 0) {
        return 0;
    }

    const size_t grant = std::min(maxBytes, appWriteCredit());
    if (grant == 0) {
        return 0;
    }

    size_t count = m_sendBuffer.writableViews(views, grant);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += views[i].len;
    }
    return total;
}

int32_t StreamImpl::commitWrite(size_t bytes, bool fin)
{
    Status st;
    if (m_localFinQueued) {
        return StreamErr(UTP_ERR_STREAM_CLOSED, st);
    }

    if (m_conn == nullptr) {
        return StreamErr(UTP_ERR_INVALID_STATE, st);
    }

    if (bytes > appWriteCredit()) {
        return StreamErr(UTP_ERR_WOULD_BLOCK, st);
    }

    if (bytes > m_sendBuffer.freeSize()) {
        return StreamErr(UTP_ERR_OVERFLOW, st);
    }

    if (bytes > (std::numeric_limits<uint32_t>::max)()) {
        return StreamErr(UTP_ERR_OVERFLOW, st);
    }

    if (m_sendQueuedBytes + m_sendInFlightBytes + bytes > kMaxSendQueueBytes) {
        return StreamErr(UTP_ERR_STREAM_DATA_LIMITED, st);
    }

    if (bytes > 0) {
        m_sendBuffer.produce(bytes);
        m_sendQueuedBytes += bytes;
        m_lastSendQueuedAtUs = time::MonotonicUs();
    }

    if (fin) {
        m_localFinQueued = true;
    }

    if (m_conn != nullptr) {
        const utp_time_t nowUs = time::MonotonicUs();
        if (shouldDeferSend(nowUs)) {
            const utp_time_t remainUs = coalesceDelayRemainingUs(nowUs);
            const utp_time_t delayMs = std::max<utp_time_t>(1, (remainUs + 999) / 1000);
            m_conn->nextScheduleTime(delayMs);
        } else {
            // Application writes only enqueue data. Actual send is driven by
            // connection writable scheduling, so app can naturally hit
            // WOULD_BLOCK once stream buffer credit is exhausted.
            m_conn->scheduleWrite();
            m_conn->nextScheduleTime(1);
        }
    }

    maybeNotifyClosed();
    return static_cast<int32_t>(bytes);
}

size_t StreamImpl::acquireReadViews(ConstBufferView views[2], size_t maxBytes) const
{
    if (views == nullptr || maxBytes == 0) {
        return 0;
    }

    views[0] = {};
    views[1] = {};

    size_t total = 0;
    size_t idx = 0;
    uint64_t expectedOffset = m_recvOffset;
    for (RecvFragment *fragment = firstFragment();
         fragment != nullptr && idx < 2 && total < maxBytes;
         fragment = nextFragment(fragment)) {
        const uint64_t logicalOffset = fragment->offset + fragment->consumed;
        if (logicalOffset != expectedOffset) {
            break;
        }

        const size_t remaining = fragment->remaining();
        if (remaining == 0) {
            if (fragment->fin) {
                break;
            }
            continue;
        }

        const size_t n = std::min(remaining, maxBytes - total);
        views[idx].data = fragment->data + fragment->consumed;
        views[idx].len = n;
        total += n;
        expectedOffset += n;
        ++idx;
    }

    return total;
}

int32_t StreamImpl::commitReadViews(size_t bytes)
{
    Status st;
    if (bytes == 0) {
        maybeAdvancePeerFin();
        maybeNotifyClosed();
        return 0;
    }

    const size_t allowed = contiguousReadableBytes(bytes);
    if (allowed < bytes) {
        return StreamErr(UTP_ERR_OVERFLOW, st);
    }

    size_t left = bytes;
    while (left > 0) {
        RecvFragment *fragment = firstFragment();
        if (fragment == nullptr || (fragment->offset + fragment->consumed) != m_recvOffset) {
            return StreamErr(UTP_ERR_OVERFLOW, st);
        }

        const size_t remaining = fragment->remaining();
        const size_t n = std::min(remaining, left);
        fragment->consumed += n;
        m_recvBufferedBytes = (m_recvBufferedBytes >= n) ? (m_recvBufferedBytes - n) : 0;
        m_recvOffset += n;
        left -= n;

        if (fragment->remaining() == 0) {
            if (fragment->fin) {
                m_peerFin = true;
            }
            eraseRecvFragment(fragment);
        }
    }

    maybeAdvancePeerFin();
    if (bytes > 0 && m_conn != nullptr) {
        m_conn->onStreamBytesConsumed(m_streamId, bytes);
    }
    maybeNotifyClosed();
    return static_cast<int32_t>(bytes);
}

Stream::State StreamImpl::state() const
{
    if (m_localFinQueued && m_localFinSent && m_peerFin) {
        return kStateClosed;
    }
    if (m_localFinQueued) {
        return kStateHalfClosedLocal;
    }
    if (m_peerFin) {
        return kStateHalfClosedRemote;
    }
    return kStateOpen;
}

bool StreamImpl::readable() const
{
    return contiguousReadableBytes(1) > 0;
}

bool StreamImpl::writable() const
{
    return !m_localFinQueued
        && !m_resetByPeer
    && appWriteCredit() > 0;
}

void StreamImpl::close()
{
    (void)write(nullptr, 0, true);
}

int32_t StreamImpl::reset(uint16_t errorCode)
{
    Status st;
    if (m_conn == nullptr) {
        return StreamErr(UTP_ERR_INVALID_STATE, st);
    }

    if (m_localFinQueued && m_localFinSent && m_peerFin) {
        return StreamErr(UTP_ERR_STREAM_CLOSED, st);
    }

    const Status status = m_conn->sendResetStreamFrame(m_streamId, errorCode, sendBufferedEndOffset());
    if (!status.ok()) {
        return StreamErr(status.code(), st);
    }

    m_localFinQueued = true;
    m_localFinSent = true;
    m_peerFin = true;
    m_sendQueuedBytes = 0;
    m_sendInFlightBytes = 0;
    m_sendAckedOffset = m_nextSendOffset;
    m_sendAckedRanges.clear();
    m_sendBuffer.consume(m_sendBuffer.size());
    clearRecvFragments();
    maybeNotifyClosed();
    return 0;
}

bool StreamImpl::resetReceived() const
{
    return m_resetByPeer;
}

int32_t StreamImpl::setPriority(uint8_t priority)
{
    Status st;
    if (priority < Stream::kPriorityHighest || priority > Stream::kPriorityLowest) {
        UTP_LOGW("stream %u setPriority invalid value: %u", m_streamId, static_cast<uint32_t>(priority));
        return StreamErr(UTP_ERR_INVALID_PARAM, st);
    }

    if (m_priority == priority) {
        return 0;
    }

    const uint8_t oldPriority = m_priority;
    m_priority = priority;
    m_schedWaitRounds = 0;
    UTP_LOGD("stream %u priority updated: %u -> %u",
             m_streamId,
             static_cast<uint32_t>(oldPriority),
             static_cast<uint32_t>(m_priority));
    if (m_conn != nullptr) {
        m_conn->scheduleWrite();
    }

    return 0;
}

uint8_t StreamImpl::priority() const
{
    return m_priority;
}

void StreamImpl::setOnReadable(const OnReadable &cb)
{
    m_onReadable = cb;
    maybeNotifyReadable(true);
}

void StreamImpl::setOnWritable(const OnWritable &cb)
{
    m_onWritable = cb;
    maybeNotifyWritable(true);
}

void StreamImpl::setOnClosed(const OnClosed &cb)
{
    m_onClosed = cb;
}

void StreamImpl::setOnReset(const OnReset &cb)
{
    m_onReset = cb;
}

Status StreamImpl::onFrame(const FrameStream &frame, PacketIn *packet)
{
    if (m_resetByPeer) {
        return Status::ErrorLiteral(UTP_ERR_STREAM_CLOSED, "stream reset by peer");
    }

    if (frame.stream_id != m_streamId) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "stream id mismatch");
    }

    if (frame.stream_data_length > 0 && frame.stream_data == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "null stream data");
    }

    uint64_t frameOffset = frame.stream_offset;
    uint16_t frameLength = frame.stream_data_length;
    const uint8_t *data = static_cast<const uint8_t *>(frame.stream_data);
    const bool frameFin = STREAM_IS_FIN(frame.stream_flag);
    if (frameOffset > (std::numeric_limits<uint64_t>::max)() - frameLength) {
        return Status::ErrorLiteral(UTP_ERR_STREAM_FLOW_CONTROL, "stream offset overflow");
    }
    if (frameOffset > m_recvOffset && m_conn != nullptr && m_conn->config() != nullptr) {
        const uint64_t maxGap = std::max<uint32_t>(m_conn->config()->recv_stream_max_gap, 1);
        if (frameOffset - m_recvOffset > maxGap) {
            return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "stream reassembly gap limit exceeded");
        }
    }
    const uint64_t frameEnd = frameOffset + frameLength;

    if (frameOffset < m_recvOffset) {
        const uint64_t trim = m_recvOffset - frameOffset;
        if (trim >= frameLength) {
            if (frameFin && frameEnd == m_recvOffset) {
                m_peerFin = true;
                maybeNotifyClosed();
            }
            return Status::OK();
        }

        frameOffset += trim;
        frameLength = static_cast<uint16_t>(frameLength - trim);
        data += trim;
    }

    RecvFragment *insertedLast = nullptr;
    const uint64_t originalEnd = frameOffset + frameLength;
    uint64_t cursor = frameOffset;
    const uint8_t *cursorData = data;

    RecvFragment *prev = findPrev(cursor);
    if (prev != nullptr) {
        const uint64_t prevEnd = prev->offset + prev->len;
        if (prevEnd > cursor) {
            const uint64_t trim = std::min<uint64_t>(prevEnd - cursor, frameLength);
            cursor += trim;
            cursorData += trim;
            frameLength = static_cast<uint16_t>(frameLength - trim);
        }
    }

    RecvFragment *iter = findLowerBound(cursor);
    while (frameLength > 0) {
        if (iter == nullptr || iter->offset >= (cursor + frameLength)) {
            RecvFragment *fragment = m_recvMm != nullptr ? m_recvMm->getRecvFragment() : nullptr;
            if (fragment == nullptr) {
                return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "failed to get recv fragment");
            }
            fragment->packet = packet;
            fragment->data = cursorData;
            fragment->len = frameLength;
            fragment->consumed = 0;
            fragment->offset = cursor;
            fragment->fin = frameFin && (cursor + frameLength == originalEnd);

            if (m_recvMm != nullptr && fragment->packet != nullptr && fragment->len > 0) {
                m_recvMm->retainPacketIn(fragment->packet);
            }

            bool inserted = false;
            Status status = insertRecvFragment(fragment, &inserted);
            if (!status.ok()) {
                releaseRecvFragment(fragment);
                return status;
            }

            if (inserted) {
                insertedLast = fragment;
            }
            break;
        }

        if (iter->offset > cursor) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(iter->offset - cursor, frameLength));
            RecvFragment *fragment = m_recvMm != nullptr ? m_recvMm->getRecvFragment() : nullptr;
            if (fragment == nullptr) {
                return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "failed to get recv fragment");
            }
            fragment->packet = packet;
            fragment->data = cursorData;
            fragment->len = chunk;
            fragment->consumed = 0;
            fragment->offset = cursor;
            fragment->fin = false;

            if (m_recvMm != nullptr && fragment->packet != nullptr && fragment->len > 0) {
                m_recvMm->retainPacketIn(fragment->packet);
            }

            bool inserted = false;
            Status status = insertRecvFragment(fragment, &inserted);
            if (!status.ok()) {
                releaseRecvFragment(fragment);
                return status;
            }

            if (inserted) {
                insertedLast = fragment;
            }
            cursor += chunk;
            cursorData += chunk;
            frameLength = static_cast<uint16_t>(frameLength - chunk);
            continue;
        }

        const uint64_t iterEnd = iter->offset + iter->len;
        if (iterEnd <= cursor) {
            iter = nextFragment(iter);
            continue;
        }

        const uint64_t trim = std::min<uint64_t>(iterEnd - cursor, frameLength);
        cursor += trim;
        cursorData += trim;
        frameLength = static_cast<uint16_t>(frameLength - trim);
        iter = nextFragment(iter);
    }

    if (frameFin) {
        if (insertedLast != nullptr) {
            insertedLast->fin = true;
        } else {
            const uint64_t tailLookup = originalEnd == (std::numeric_limits<uint64_t>::max)()
                                            ? originalEnd
                                            : originalEnd + 1;
            RecvFragment *tail = findPrev(tailLookup);
            if (tail != nullptr && tail->offset + tail->len == originalEnd) {
                tail->fin = true;
            } else if (originalEnd == m_recvOffset) {
                m_peerFin = true;
            } else {
                RecvFragment *fragment = m_recvMm != nullptr ? m_recvMm->getRecvFragment() : nullptr;
                if (fragment == nullptr) {
                    return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "failed to get recv fragment");
                }
                fragment->packet = packet;
                fragment->data = nullptr;
                fragment->len = 0;
                fragment->consumed = 0;
                fragment->offset = originalEnd;
                fragment->fin = true;
                bool inserted = false;
                Status status = insertRecvFragment(fragment, &inserted);
                if (!status.ok()) {
                    releaseRecvFragment(fragment);
                    return status;
                }
            }
        }
    }

    maybeAdvancePeerFin();
    maybeNotifyReadable(true);
    maybeNotifyClosed();
    return Status::OK();
}

Status StreamImpl::onReset(uint16_t errorCode, bool fromPeer)
{
    m_resetErrorCode = errorCode;
    if (fromPeer) {
        m_resetByPeer = true;
    }

    m_sendQueuedBytes = 0;
    m_sendInFlightBytes = 0;
    m_sendAckedOffset = m_nextSendOffset;
    m_sendAckedRanges.clear();
    m_sendBuffer.consume(m_sendBuffer.size());
    clearRecvFragments();
    m_localFinQueued = true;
    m_localFinSent = true;
    m_peerFin = true;

    notifyResetOnce();
    maybeNotifyClosed();
    return Status::OK();
}

Status StreamImpl::flushPendingSends(size_t maxBytes)
{
    if (m_conn == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_STATE, "null connection");
    }

    size_t sentBytes = 0;
    while (sentBytes < maxBytes) {
        const uint64_t inFlightBytesU64 = (m_nextSendOffset >= m_sendAckedOffset)
                                       ? (m_nextSendOffset - m_sendAckedOffset)
                                       : 0;
        const size_t inFlightBytes = static_cast<size_t>(std::min<uint64_t>(inFlightBytesU64, m_sendBuffer.size()));
        const size_t maxQueuedFromBuffer = m_sendBuffer.size() > inFlightBytes
                                         ? (m_sendBuffer.size() - inFlightBytes)
                                         : 0;
        if (m_sendQueuedBytes > maxQueuedFromBuffer) {
            m_sendQueuedBytes = maxQueuedFromBuffer;
        }

        const uint64_t queuedEndOffset = m_nextSendOffset + static_cast<uint64_t>(m_sendQueuedBytes);
        const uint64_t unsentBytesU64 = queuedEndOffset > m_nextSendOffset
                                     ? (queuedEndOffset - m_nextSendOffset)
                                     : 0;
        const size_t unsentBytes = static_cast<size_t>(std::min<uint64_t>(unsentBytesU64, (std::numeric_limits<size_t>::max)()));

        if (unsentBytes == 0) {
            if (!(m_localFinQueued && !m_localFinSent)) {
                break;
            }

            const Status finStatus = m_conn->sendStreamFrame(m_streamId,
                                                              m_nextSendOffset,
                                                              nullptr,
                                                              0,
                                                              true);
            if (finStatus.ok()) {
                m_localFinSent = true;
                continue;
            }

            if (finStatus.code() == UTP_ERR_WOULD_BLOCK) {
                m_conn->scheduleWrite();
                UTP_LOGD("stream %u send blocked, queue_bytes=%zu", m_streamId, m_sendQueuedBytes);
            } else {
                UTP_LOGW("stream %u flushPendingSends failed: status=%d", m_streamId, finStatus.code());
            }
            return finStatus;
        }

        const size_t budgetLeft = maxBytes - sentBytes;
        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        bool frameFin = false;

        ConstBufferView views[2];
        const size_t startOffsetInBuffer = inFlightBytes;
        const size_t count = m_sendBuffer.readableViewsFrom(views,
                                                            startOffsetInBuffer,
                                                            std::min(unsentBytes, budgetLeft));
        if (count == 0 || views[0].data == nullptr || views[0].len == 0) {
            if (unsentBytes == 0) {
                continue;
            }
            return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "no readable views");
        }

        payload = static_cast<const uint8_t *>(views[0].data);
        payloadLen = std::min<size_t>(views[0].len, UINT16_MAX);

        Status status = Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "initial block");
        size_t tryLen = std::min(payloadLen, StreamPayloadMtuBudget(m_conn));
        while (tryLen > 0) {
            frameFin = m_localFinQueued && !m_localFinSent && (tryLen == unsentBytes);
            status = m_conn->sendStreamFrame(m_streamId,
                                             m_nextSendOffset,
                                             payload,
                                             tryLen,
                                             frameFin);
            if (status.code() != UTP_ERR_WOULD_BLOCK || tryLen == 1) {
                payloadLen = tryLen;
                break;
            }

            // Coalesced payload may exceed current path/anti-amplification budget.
            // Retry with smaller chunks to guarantee forward progress.
            tryLen /= 2;
        }

        if (status.ok()) {
            m_nextSendOffset += payloadLen;
            if (m_sendQueuedBytes >= payloadLen) {
                m_sendQueuedBytes -= payloadLen;
            } else {
                m_sendQueuedBytes = 0;
            }
            m_sendInFlightBytes += payloadLen;
            sentBytes += payloadLen;

            if (frameFin) {
                m_localFinSent = true;
            }
            continue;
        }

        if (status.code() == UTP_ERR_WOULD_BLOCK) {
            m_conn->scheduleWrite();
            UTP_LOGD("stream %u send blocked, queue_bytes=%zu", m_streamId, m_sendQueuedBytes);
        }
        if (!status.ok() && status.code() != UTP_ERR_WOULD_BLOCK) {
            UTP_LOGW("stream %u flushPendingSends failed: status=%d", m_streamId, status.code());
        }
        return status;
    }

    return Status::OK();
}

Status StreamImpl::onConnectionWritable(utp_time_t nowUs)
{
    if (m_conn != nullptr) {
        if (shouldDeferSend(nowUs)) {
            const utp_time_t remainUs = coalesceDelayRemainingUs(nowUs);
            const utp_time_t delayMs = std::max<utp_time_t>(1, (remainUs + 999) / 1000);
            m_conn->nextScheduleTime(delayMs);
            return Status::OK();
        }
    }

    const Status status = flushPendingSends(StreamWritableSendBudget(m_conn));
    if (!status.ok() && status.code() != UTP_ERR_WOULD_BLOCK) {
        return status;
    }

    maybeNotifyWritable(true);
    return status;
}

void StreamImpl::onPacketAcked(uint64_t streamOffset, size_t len)
{
    if (len == 0) {
        return;
    }

    uint64_t start = streamOffset;
    uint64_t end = streamOffset + static_cast<uint64_t>(len);
    if (end <= m_sendAckedOffset) {
        return;
    }
    if (start < m_sendAckedOffset) {
        start = m_sendAckedOffset;
    }

    auto it = m_sendAckedRanges.lower_bound(start);
    if (it != m_sendAckedRanges.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= start) {
            start = prev->first;
            end = std::max(end, prev->second);
            it = m_sendAckedRanges.erase(prev);
        }
    }

    while (it != m_sendAckedRanges.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = m_sendAckedRanges.erase(it);
    }
    m_sendAckedRanges[start] = end;

    uint64_t advancedTo = m_sendAckedOffset;
    auto begin = m_sendAckedRanges.begin();
    if (begin != m_sendAckedRanges.end() && begin->first <= m_sendAckedOffset && begin->second > m_sendAckedOffset) {
        advancedTo = begin->second;
    }

    if (advancedTo > m_sendAckedOffset) {
        const size_t delta = static_cast<size_t>(std::min<uint64_t>(advancedTo - m_sendAckedOffset,
                                                                     m_sendBuffer.size()));
        if (delta > 0) {
            m_sendBuffer.consume(delta);
            m_sendAckedOffset += delta;
            if (m_sendInFlightBytes >= delta) {
                m_sendInFlightBytes -= delta;
            } else {
                m_sendInFlightBytes = 0;
            }
            maybeNotifyWritable(true);
        }
    }

    while (!m_sendAckedRanges.empty()) {
        auto front = m_sendAckedRanges.begin();
        if (front->second <= m_sendAckedOffset) {
            m_sendAckedRanges.erase(front);
            continue;
        }
        break;
    }
}

size_t StreamImpl::appWriteCredit() const
{
    const size_t cap = StreamSendBufferLimit(m_conn);
    const size_t used = m_sendQueuedBytes + m_sendInFlightBytes;
    const size_t queueCredit = (used >= cap) ? 0 : (cap - used);
    return std::min(queueCredit, m_sendBuffer.freeSize());
}

uint64_t StreamImpl::sendBufferedEndOffset() const
{
    return m_nextSendOffset + static_cast<uint64_t>(m_sendQueuedBytes);
}

bool StreamImpl::hasPendingSendWork() const
{
    return m_sendQueuedBytes > 0 || (m_localFinQueued && !m_localFinSent);
}

bool StreamImpl::shouldDeferSend(utp_time_t nowUs) const
{
    if (!hasPendingSendWork()) {
        return false;
    }

    if (m_localFinQueued) {
        return false;
    }

    if (m_conn == nullptr) {
        return false;
    }

    // Before path validation and HandshakeDone convergence, anti-amplification
    // credit is tight. Deferring here can accumulate payloads and repeatedly
    // hit WOULD_BLOCK without making stream-level forward progress.
    if (m_conn->m_networkPath.needPathValidation()) {
        return false;
    }
    if (m_conn->m_handshakeDonePending && !m_conn->m_handshakeDoneSent) {
        return false;
    }
    if (m_conn->m_bytesIn < 2048) {
        return false;
    }

    const Config *cfg = m_conn->config();
    if (cfg == nullptr || !cfg->stream_enable_coalescing) {
        return false;
    }

    if (cfg->stream_coalesce_delay_us == 0) {
        return false;
    }

    if (m_sendQueuedBytes >= cfg->stream_min_payload_before_immediate_send) {
        return false;
    }

    if (m_lastSendQueuedAtUs == 0) {
        return false;
    }

    return nowUs < (m_lastSendQueuedAtUs + cfg->stream_coalesce_delay_us);
}

utp_time_t StreamImpl::coalesceDelayRemainingUs(utp_time_t nowUs) const
{
    if (m_conn == nullptr) {
        return 0;
    }

    const Config *cfg = m_conn->config();
    if (cfg == nullptr || m_lastSendQueuedAtUs == 0) {
        return 0;
    }

    const utp_time_t deadlineUs = m_lastSendQueuedAtUs + cfg->stream_coalesce_delay_us;
    if (nowUs >= deadlineUs) {
        return 0;
    }

    return deadlineUs - nowUs;
}

Status StreamImpl::insertRecvFragment(RecvFragment *fragment, bool *inserted)
{
    if (fragment == nullptr) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "null fragment");
    }

    if (inserted != nullptr) {
        *inserted = false;
    }

    if (fragment->len > 0 && m_recvBufferedBytes + fragment->len > kMaxRecvFragmentBytes) {
        return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "recv buffer full");
    }

    struct rb_node **link = &m_recvFragmentsTree.rb_node;
    struct rb_node *parent = nullptr;
    while (*link != nullptr) {
        RecvFragment *current = rb_entry(*link, RecvFragment, treeNode);
        parent = *link;
        if (fragment->offset < current->offset) {
            link = &(*link)->rb_left;
        } else if (fragment->offset > current->offset) {
            link = &(*link)->rb_right;
        } else {
            if (current->len >= fragment->len) {
                releaseRecvFragment(fragment);
                return Status::OK();
            }

            if (!accountRecvFragment(fragment, current)) {
                return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "recv fragment memory limit exceeded");
            }

            rb_replace_node(&current->treeNode, &fragment->treeNode, &m_recvFragmentsTree);
            m_recvBufferedBytes = m_recvBufferedBytes - current->remaining() + fragment->remaining();
            releaseRecvFragment(current);
            if (inserted != nullptr) {
                *inserted = true;
            }
            return Status::OK();
        }
    }

    if (!accountRecvFragment(fragment)) {
        return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "recv fragment memory limit exceeded");
    }

    rb_link_node(&fragment->treeNode, parent, link);
    rb_insert_color(&fragment->treeNode, &m_recvFragmentsTree);
    m_recvBufferedBytes += fragment->remaining();
    if (inserted != nullptr) {
        *inserted = true;
    }
    return Status::OK();
}

void StreamImpl::clearRecvFragments()
{
    RecvFragment *fragment = firstFragment();
    while (fragment != nullptr) {
        RecvFragment *next = nextFragment(fragment);
        rb_erase(&fragment->treeNode, &m_recvFragmentsTree);
        releaseRecvFragment(fragment);
        fragment = next;
    }
    m_recvFragmentsTree = RB_ROOT;
    m_recvBufferedBytes = 0;
}

RecvFragment* StreamImpl::findLowerBound(uint64_t offset) const
{
    struct rb_node *node = m_recvFragmentsTree.rb_node;
    RecvFragment *candidate = nullptr;
    while (node != nullptr) {
        RecvFragment *fragment = rb_entry(node, RecvFragment, treeNode);
        if (fragment->offset < offset) {
            node = node->rb_right;
        } else {
            candidate = fragment;
            node = node->rb_left;
        }
    }
    return candidate;
}

RecvFragment* StreamImpl::findPrev(uint64_t offset) const
{
    RecvFragment *candidate = nullptr;
    struct rb_node *node = m_recvFragmentsTree.rb_node;
    while (node != nullptr) {
        RecvFragment *fragment = rb_entry(node, RecvFragment, treeNode);
        if (fragment->offset < offset) {
            candidate = fragment;
            node = node->rb_right;
        } else {
            node = node->rb_left;
        }
    }
    return candidate;
}

RecvFragment* StreamImpl::firstFragment() const
{
    struct rb_node *node = rb_first(const_cast<struct rb_root *>(&m_recvFragmentsTree));
    return node == nullptr ? nullptr : rb_entry(node, RecvFragment, treeNode);
}

RecvFragment* StreamImpl::nextFragment(const RecvFragment *fragment) const
{
    if (fragment == nullptr) {
        return nullptr;
    }

    struct rb_node *node = rb_next(const_cast<struct rb_node *>(&fragment->treeNode));
    return node == nullptr ? nullptr : rb_entry(node, RecvFragment, treeNode);
}

void StreamImpl::eraseRecvFragment(RecvFragment *fragment)
{
    if (fragment == nullptr) {
        return;
    }

    rb_erase(&fragment->treeNode, &m_recvFragmentsTree);
    const size_t rem = fragment->remaining();
    if (m_recvBufferedBytes >= rem) {
        m_recvBufferedBytes -= rem;
    } else {
        m_recvBufferedBytes = 0;
    }
    releaseRecvFragment(fragment);
}

void StreamImpl::releaseRecvFragment(RecvFragment *fragment)
{
    if (fragment == nullptr) {
        return;
    }

    unaccountRecvFragment(fragment);
    if (m_recvMm != nullptr && fragment->packet != nullptr && fragment->len > 0) {
        m_recvMm->releasePacketIn(fragment->packet);
    }
    fragment->packet = nullptr;
    if (m_recvMm != nullptr) {
        m_recvMm->putRecvFragment(fragment);
    }
}

bool StreamImpl::accountRecvFragment(RecvFragment *fragment, RecvFragment *replaced)
{
    if (fragment == nullptr || fragment->accounted) {
        return fragment != nullptr;
    }

    const bool replacingAccounted = replaced != nullptr && replaced->accounted;
    const size_t oldCost = replacingAccounted ? replaced->memoryCost : 0;
    const size_t packetBytes = fragment->packet != nullptr && fragment->len > 0
                                   ? static_cast<size_t>(fragment->packet->alloc_size)
                                   : 0;
    const size_t newCost = sizeof(RecvFragment) + packetBytes;
    const size_t streamBase = m_recvPinnedMemoryBytes >= oldCost ? m_recvPinnedMemoryBytes - oldCost : 0;
    const size_t streamCountBase = m_recvFragmentCount >= (replacingAccounted ? 1u : 0u)
                                       ? m_recvFragmentCount - (replacingAccounted ? 1u : 0u)
                                       : 0;

    const Config *cfg = m_conn != nullptr ? m_conn->config() : nullptr;
    const size_t streamMemoryLimit = cfg != nullptr ? std::max<uint32_t>(cfg->recv_stream_reassembly_memory_limit, 1)
                                                    : 4u * 1024u * 1024u;
    const size_t streamFragmentLimit = cfg != nullptr
                                           ? std::max<uint32_t>(cfg->recv_stream_reassembly_fragment_limit, 1)
                                           : 1024u;
    if (streamBase > streamMemoryLimit || newCost > streamMemoryLimit - streamBase ||
        streamCountBase >= streamFragmentLimit) {
        return false;
    }

    size_t connBase = 0;
    size_t connCountBase = 0;
    if (m_conn != nullptr) {
        connBase = m_conn->m_recvReassemblyMemoryBytes >= oldCost
                       ? m_conn->m_recvReassemblyMemoryBytes - oldCost
                       : 0;
        connCountBase = m_conn->m_recvReassemblyFragmentCount >= (replacingAccounted ? 1u : 0u)
                            ? m_conn->m_recvReassemblyFragmentCount - (replacingAccounted ? 1u : 0u)
                            : 0;
        const size_t connMemoryLimit = std::max<uint32_t>(cfg->recv_reassembly_memory_limit, 1);
        const size_t connFragmentLimit = std::max<uint32_t>(cfg->recv_reassembly_fragment_limit, 1);
        if (connBase > connMemoryLimit || newCost > connMemoryLimit - connBase ||
            connCountBase >= connFragmentLimit) {
            return false;
        }
    }

    if (replacingAccounted) {
        replaced->accounted = false;
        replaced->memoryCost = 0;
    }
    m_recvPinnedMemoryBytes = streamBase + newCost;
    m_recvFragmentCount = streamCountBase + 1;
    if (m_conn != nullptr) {
        m_conn->m_recvReassemblyMemoryBytes = connBase + newCost;
        m_conn->m_recvReassemblyFragmentCount = connCountBase + 1;
    }
    fragment->accounted = true;
    fragment->memoryCost = newCost;
    return true;
}

void StreamImpl::unaccountRecvFragment(RecvFragment *fragment)
{
    if (fragment == nullptr || !fragment->accounted) {
        return;
    }

    const size_t cost = fragment->memoryCost;
    m_recvPinnedMemoryBytes = m_recvPinnedMemoryBytes >= cost ? m_recvPinnedMemoryBytes - cost : 0;
    if (m_recvFragmentCount > 0) {
        --m_recvFragmentCount;
    }
    if (m_conn != nullptr) {
        m_conn->m_recvReassemblyMemoryBytes = m_conn->m_recvReassemblyMemoryBytes >= cost
                                                  ? m_conn->m_recvReassemblyMemoryBytes - cost
                                                  : 0;
        if (m_conn->m_recvReassemblyFragmentCount > 0) {
            --m_conn->m_recvReassemblyFragmentCount;
        }
    }
    fragment->accounted = false;
    fragment->memoryCost = 0;
}

void StreamImpl::maybeAdvancePeerFin()
{
    while (true) {
        RecvFragment *fragment = firstFragment();
        if (fragment == nullptr
            || (fragment->offset + fragment->consumed) != m_recvOffset
            || fragment->remaining() > 0) {
            break;
        }

        if (fragment->fin) {
            m_peerFin = true;
        }
        eraseRecvFragment(fragment);
    }
}

size_t StreamImpl::contiguousReadableBytes(size_t maxBytes) const
{
    if (maxBytes == 0) {
        return 0;
    }

    size_t total = 0;
    uint64_t expectedOffset = m_recvOffset;
    for (RecvFragment *fragment = firstFragment();
         fragment != nullptr && total < maxBytes;
         fragment = nextFragment(fragment)) {
        const uint64_t logicalOffset = fragment->offset + fragment->consumed;
        if (logicalOffset != expectedOffset) {
            break;
        }

        const size_t rem = fragment->remaining();
        if (rem == 0) {
            if (fragment->fin) {
                break;
            }
            continue;
        }

        const size_t n = std::min(rem, maxBytes - total);
        total += n;
        expectedOffset += n;
    }

    return total;
}

void StreamImpl::maybeNotifyClosed()
{
    if (m_closedNotified) {
        return;
    }

    if (m_localFinQueued && m_localFinSent && m_peerFin && m_recvBufferedBytes == 0) {
        m_closedNotified = true;
        if (m_onClosed) {
            m_onClosed();
        }
    }
}

void StreamImpl::maybeNotifyReadable(bool allowPeerFin)
{
    if (!m_onReadable || m_notifyingReadable) {
        return;
    }

    if (!readable() && !(allowPeerFin && m_peerFin)) {
        return;
    }

    m_notifyingReadable = true;
    m_onReadable();
    m_notifyingReadable = false;
}

void StreamImpl::maybeNotifyWritable(bool force)
{
    if (!force || !m_onWritable || m_notifyingWritable || !writable()) {
        return;
    }

    m_notifyingWritable = true;
    m_onWritable();
    m_notifyingWritable = false;
}

void StreamImpl::notifyResetOnce()
{
    if (m_resetNotified) {
        return;
    }

    if (m_onReset) {
        m_onReset(m_resetErrorCode);
    }
    m_resetNotified = true;
}

} // namespace utp
} // namespace eular
