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

static inline int32_t StreamErr(utp_error_t err)
{
    if (err == UTP_ERR_OK) {
        return UTP_ERR_OK;
    }

    SetLastErrorV(err, "stream operation failed: {}", static_cast<uint32_t>(err));
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
    if (len == 0 && !fin) {
        return StreamErr(UTP_ERR_INVALID_PARAM);
    }

    if (m_localFinQueued) {
        return StreamErr(UTP_ERR_STREAM_CLOSED);
    }

    if (len > appWriteCredit()) {
        return StreamErr(UTP_ERR_WOULD_BLOCK);
    }

    MutableBufferView views[2];
    size_t acquired = acquireWriteBuffer(views, len);
    if (acquired < len) {
        return StreamErr(UTP_ERR_WOULD_BLOCK);
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
    if (buffer == nullptr || capacity == 0) {
        return StreamErr(UTP_ERR_INVALID_PARAM);
    }

    const size_t contiguous = contiguousReadableBytes(capacity);
    if (contiguous == 0) {
        maybeAdvancePeerFin();
        return m_peerFin ? 0 : StreamErr(UTP_ERR_WOULD_BLOCK);
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
    if (m_localFinQueued) {
        return StreamErr(UTP_ERR_STREAM_CLOSED);
    }

    if (m_conn == nullptr) {
        return StreamErr(UTP_ERR_INVALID_STATE);
    }

    if (bytes > appWriteCredit()) {
        return StreamErr(UTP_ERR_WOULD_BLOCK);
    }

    if (bytes > m_sendBuffer.freeSize()) {
        return StreamErr(UTP_ERR_OVERFLOW);
    }

    if (bytes > (std::numeric_limits<uint32_t>::max)()) {
        return StreamErr(UTP_ERR_OVERFLOW);
    }

    if (m_sendQueuedBytes + m_sendInFlightBytes + bytes > kMaxSendQueueBytes) {
        return StreamErr(UTP_ERR_STREAM_DATA_LIMITED);
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
    if (bytes == 0) {
        maybeAdvancePeerFin();
        maybeNotifyClosed();
        return UTP_ERR_OK;
    }

    const size_t allowed = contiguousReadableBytes(bytes);
    if (allowed < bytes) {
        return StreamErr(UTP_ERR_OVERFLOW);
    }

    size_t left = bytes;
    while (left > 0) {
        RecvFragment *fragment = firstFragment();
        if (fragment == nullptr || (fragment->offset + fragment->consumed) != m_recvOffset) {
            return StreamErr(UTP_ERR_OVERFLOW);
        }

        const size_t remaining = fragment->remaining();
        const size_t n = std::min(remaining, left);
        fragment->consumed += n;
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
    if (m_conn == nullptr) {
        return StreamErr(UTP_ERR_INVALID_STATE);
    }

    if (m_localFinQueued && m_localFinSent && m_peerFin) {
        return StreamErr(UTP_ERR_STREAM_CLOSED);
    }

    const int32_t status = m_conn->sendResetStreamFrame(m_streamId, errorCode, sendBufferedEndOffset());
    if (status != UTP_ERR_OK) {
        if (status < 0) {
            return StreamErr(UTP_ERR_INTERNAL_ERROR);
        }
        return StreamErr(static_cast<utp_error_t>(status));
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
    return UTP_ERR_OK;
}

bool StreamImpl::resetReceived() const
{
    return m_resetByPeer;
}

int32_t StreamImpl::setPriority(uint8_t priority)
{
    if (priority < Stream::kPriorityHighest || priority > Stream::kPriorityLowest) {
        UTP_LOGW("stream %u setPriority invalid value: %u", m_streamId, static_cast<uint32_t>(priority));
        return StreamErr(UTP_ERR_INVALID_PARAM);
    }

    if (m_priority == priority) {
        return UTP_ERR_OK;
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

    return UTP_ERR_OK;
}

uint8_t StreamImpl::priority() const
{
    return m_priority;
}

void StreamImpl::setOnReadable(const OnReadable &cb)
{
    m_onReadable = cb;
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

int32_t StreamImpl::onFrame(const FrameStream &frame, PacketIn *packet)
{
    if (m_resetByPeer) {
        return UTP_ERR_STREAM_CLOSED;
    }

    if (frame.stream_id != m_streamId) {
        return UTP_ERR_INVALID_PARAM;
    }

    if (frame.stream_data_length > 0 && frame.stream_data == nullptr) {
        return UTP_ERR_INVALID_PARAM;
    }

    uint64_t frameOffset = frame.stream_offset;
    uint16_t frameLength = frame.stream_data_length;
    const uint8_t *data = static_cast<const uint8_t *>(frame.stream_data);
    const bool frameFin = STREAM_IS_FIN(frame.stream_flag);
    const uint64_t frameEnd = frameOffset + frameLength;

    if (frameOffset < m_recvOffset) {
        const uint64_t trim = m_recvOffset - frameOffset;
        if (trim >= frameLength) {
            if (frameFin && frameEnd == m_recvOffset) {
                m_peerFin = true;
                maybeNotifyClosed();
            }
            return UTP_ERR_OK;
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
                return UTP_ERR_INTERNAL_ERROR;
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
            int32_t status = insertRecvFragment(fragment, &inserted);
            if (status != UTP_ERR_OK) {
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
                return UTP_ERR_INTERNAL_ERROR;
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
            int32_t status = insertRecvFragment(fragment, &inserted);
            if (status != UTP_ERR_OK) {
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
            RecvFragment *tail = findPrev(originalEnd + 1);
            if (tail != nullptr && tail->offset + tail->len == originalEnd) {
                tail->fin = true;
            } else if (originalEnd == m_recvOffset) {
                m_peerFin = true;
            } else {
                RecvFragment *fragment = m_recvMm != nullptr ? m_recvMm->getRecvFragment() : nullptr;
                if (fragment == nullptr) {
                    return UTP_ERR_INTERNAL_ERROR;
                }
                fragment->packet = packet;
                fragment->data = nullptr;
                fragment->len = 0;
                fragment->consumed = 0;
                fragment->offset = originalEnd;
                fragment->fin = true;
                bool inserted = false;
                int32_t status = insertRecvFragment(fragment, &inserted);
                if (status != UTP_ERR_OK) {
                    releaseRecvFragment(fragment);
                    return status;
                }
            }
        }
    }

    maybeAdvancePeerFin();
    if (m_onReadable && readable()) {
        m_onReadable();
    }

    maybeNotifyClosed();
    return UTP_ERR_OK;
}

int32_t StreamImpl::onReset(uint16_t errorCode, bool fromPeer)
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
    return UTP_ERR_OK;
}

int32_t StreamImpl::flushPendingSends(size_t maxBytes)
{
    if (m_conn == nullptr) {
        return UTP_ERR_INVALID_STATE;
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

            const int32_t finStatus = m_conn->sendStreamFrame(m_streamId,
                                                              m_nextSendOffset,
                                                              nullptr,
                                                              0,
                                                              true);
            if (finStatus == UTP_ERR_OK) {
                m_localFinSent = true;
                continue;
            }

            if (finStatus == UTP_ERR_WOULD_BLOCK) {
                m_conn->scheduleWrite();
                UTP_LOGD("stream %u send blocked, queue_bytes=%zu", m_streamId, m_sendQueuedBytes);
            } else {
                UTP_LOGW("stream %u flushPendingSends failed: status=%d", m_streamId, finStatus);
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
            return UTP_ERR_WOULD_BLOCK;
        }

        payload = static_cast<const uint8_t *>(views[0].data);
        payloadLen = std::min<size_t>(views[0].len, UINT16_MAX);

        int32_t status = UTP_ERR_WOULD_BLOCK;
        size_t tryLen = std::min(payloadLen, StreamPayloadMtuBudget(m_conn));
        while (tryLen > 0) {
            frameFin = m_localFinQueued && !m_localFinSent && (tryLen == unsentBytes);
            status = m_conn->sendStreamFrame(m_streamId,
                                             m_nextSendOffset,
                                             payload,
                                             tryLen,
                                             frameFin);
            if (status != UTP_ERR_WOULD_BLOCK || tryLen == 1) {
                payloadLen = tryLen;
                break;
            }

            // Coalesced payload may exceed current path/anti-amplification budget.
            // Retry with smaller chunks to guarantee forward progress.
            tryLen /= 2;
        }

        if (status == UTP_ERR_OK) {
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

        if (status == UTP_ERR_WOULD_BLOCK) {
            m_conn->scheduleWrite();
            UTP_LOGD("stream %u send blocked, queue_bytes=%zu", m_streamId, m_sendQueuedBytes);
        }
        if (status != UTP_ERR_OK && status != UTP_ERR_WOULD_BLOCK) {
            UTP_LOGW("stream %u flushPendingSends failed: status=%d", m_streamId, status);
        }
        return status;
    }

    return UTP_ERR_OK;
}

int32_t StreamImpl::onConnectionWritable()
{
    if (m_conn != nullptr) {
        const utp_time_t nowUs = time::MonotonicUs();
        if (shouldDeferSend(nowUs)) {
            const utp_time_t remainUs = coalesceDelayRemainingUs(nowUs);
            const utp_time_t delayMs = std::max<utp_time_t>(1, (remainUs + 999) / 1000);
            m_conn->nextScheduleTime(delayMs);
            return UTP_ERR_OK;
        }
    }

    const int32_t status = flushPendingSends(StreamWritableSendBudget(m_conn));
    if (status != UTP_ERR_OK && status != UTP_ERR_WOULD_BLOCK) {
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

int32_t StreamImpl::insertRecvFragment(RecvFragment *fragment, bool *inserted)
{
    if (fragment == nullptr) {
        return UTP_ERR_INVALID_PARAM;
    }

    if (inserted != nullptr) {
        *inserted = false;
    }

    if (fragment->len > 0 && m_recvBufferedBytes + fragment->len > kMaxRecvFragmentBytes) {
        return UTP_ERR_WOULD_BLOCK;
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
                return UTP_ERR_OK;
            }

            rb_replace_node(&current->treeNode, &fragment->treeNode, &m_recvFragmentsTree);
            m_recvBufferedBytes = m_recvBufferedBytes - current->remaining() + fragment->remaining();
            releaseRecvFragment(current);
            if (inserted != nullptr) {
                *inserted = true;
            }
            return UTP_ERR_OK;
        }
    }

    rb_link_node(&fragment->treeNode, parent, link);
    rb_insert_color(&fragment->treeNode, &m_recvFragmentsTree);
    m_recvBufferedBytes += fragment->remaining();
    if (inserted != nullptr) {
        *inserted = true;
    }
    return UTP_ERR_OK;
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

    if (m_recvMm != nullptr && fragment->packet != nullptr && fragment->len > 0) {
        m_recvMm->releasePacketIn(fragment->packet);
    }
    fragment->packet = nullptr;
    if (m_recvMm != nullptr) {
        m_recvMm->putRecvFragment(fragment);
    }
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
