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

namespace eular {
namespace utp {

static inline int32_t StreamErr(utp_error_t err)
{
    return -static_cast<int32_t>(err);
}

StreamImpl::RingBuffer::RingBuffer(size_t capacity) :
    m_buffer(std::max<size_t>(capacity, 1), 0)
{
}

void StreamImpl::RingBuffer::ensureFree(size_t freeBytes)
{
    if (freeSize() >= freeBytes) {
        return;
    }

    const size_t required = m_size + freeBytes;
    size_t newCap = std::max<std::size_t>(m_buffer.empty() ? 1 : m_buffer.size(), 1);
    while (newCap < required) {
        newCap *= 2;
    }

    std::vector<uint8_t> newBuffer(newCap, 0);
    ConstBufferView views[2];
    const size_t count = readableViews(views, m_size);
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
        if (views[i].data != nullptr && views[i].len > 0) {
            std::memcpy(newBuffer.data() + copied, views[i].data, views[i].len);
            copied += views[i].len;
        }
    }

    m_buffer.swap(newBuffer);
    m_head = 0;
}

size_t StreamImpl::RingBuffer::readableViews(ConstBufferView views[2], size_t maxBytes) const
{
    if (views == nullptr) {
        return 0;
    }

    views[0] = {};
    views[1] = {};
    if (m_buffer.empty() || m_size == 0 || maxBytes == 0) {
        return 0;
    }

    const size_t bytes = std::min(maxBytes, m_size);
    const size_t first = std::min(bytes, m_buffer.size() - m_head);
    views[0].data = m_buffer.data() + m_head;
    views[0].len = first;

    const size_t second = bytes - first;
    if (second > 0) {
        views[1].data = m_buffer.data();
        views[1].len = second;
        return 2;
    }

    return 1;
}

size_t StreamImpl::RingBuffer::writableViews(MutableBufferView views[2], size_t maxBytes)
{
    if (views == nullptr) {
        return 0;
    }

    views[0] = {};
    views[1] = {};
    if (m_buffer.empty() || maxBytes == 0 || freeSize() == 0) {
        return 0;
    }

    const size_t bytes = std::min(maxBytes, freeSize());
    const size_t tail = (m_head + m_size) % m_buffer.size();
    const size_t first = std::min(bytes, m_buffer.size() - tail);
    views[0].data = m_buffer.data() + tail;
    views[0].len = first;

    const size_t second = bytes - first;
    if (second > 0) {
        views[1].data = m_buffer.data();
        views[1].len = second;
        return 2;
    }

    return 1;
}

void StreamImpl::RingBuffer::produce(size_t bytes)
{
    m_size = std::min(m_size + bytes, m_buffer.size());
}

void StreamImpl::RingBuffer::consume(size_t bytes)
{
    const size_t n = std::min(bytes, m_size);
    if (!m_buffer.empty()) {
        m_head = (m_head + n) % m_buffer.size();
    }
    m_size -= n;
}

size_t StreamImpl::RingBuffer::write(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    ensureFree(len);
    MutableBufferView views[2];
    const size_t count = writableViews(views, len);
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        if (views[i].data != nullptr && views[i].len > 0) {
            std::memcpy(views[i].data, data + written, views[i].len);
            written += views[i].len;
        }
    }
    produce(written);
    return written;
}

size_t StreamImpl::RingBuffer::read(uint8_t *buffer, size_t len)
{
    if (buffer == nullptr || len == 0) {
        return 0;
    }

    ConstBufferView views[2];
    const size_t count = readableViews(views, len);
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
        if (views[i].data != nullptr && views[i].len > 0) {
            std::memcpy(buffer + copied, views[i].data, views[i].len);
            copied += views[i].len;
        }
    }
    consume(copied);
    return copied;
}

StreamImpl::StreamImpl(ConnectionImpl *conn, uint32_t streamId) :
    m_conn(conn),
    m_streamId(streamId),
    m_sendBuffer(kDefaultBufferCapacity),
    m_recvBuffer(kDefaultBufferCapacity)
{
}

StreamImpl::~StreamImpl() = default;

uint32_t StreamImpl::id() const
{
    return m_streamId;
}

int32_t StreamImpl::write(const void *data, size_t len, bool fin)
{
    if (len == 0 && !fin) {
        return StreamErr(UTP_ERR_INVALID_PARAM);
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

    if (m_recvBuffer.empty()) {
        return m_peerFin ? 0 : StreamErr(UTP_ERR_WOULD_BLOCK);
    }

    const size_t n = m_recvBuffer.read(static_cast<uint8_t *>(buffer), capacity);

    maybeNotifyClosed();
    return static_cast<int32_t>(n);
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

    m_sendBuffer.ensureFree(grant);
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

    if (bytes > std::numeric_limits<uint32_t>::max()) {
        return StreamErr(UTP_ERR_OVERFLOW);
    }

    if (m_sendQueuedBytes + bytes > kMaxSendQueueBytes) {
        return StreamErr(UTP_ERR_STREAM_DATA_LIMITED);
    }

    if (bytes > 0) {
        m_sendBuffer.produce(bytes);

        PendingSendChunk chunk;
        chunk.offset = m_sendBufferedOffset;
        chunk.bytes = bytes;
        chunk.fin = false;
        m_sendQueue.push_back(chunk);
        m_sendBufferedOffset += bytes;
        m_sendQueuedBytes += bytes;
    }

    if (fin) {
        m_localFinQueued = true;
        if (!m_sendQueue.empty()) {
            m_sendQueue.back().fin = true;
        } else {
            PendingSendChunk chunk;
            chunk.offset = m_sendBufferedOffset;
            chunk.bytes = 0;
            chunk.fin = true;
            m_sendQueue.push_back(chunk);
        }
    }

    const int32_t flushStatus = flushPendingSends();
    if (flushStatus != UTP_ERR_OK && flushStatus != UTP_ERR_WOULD_BLOCK) {
        return StreamErr(static_cast<utp_error_t>(flushStatus));
    }

    maybeNotifyClosed();
    return static_cast<int32_t>(bytes);
}

size_t StreamImpl::acquireReadBuffer(ConstBufferView views[2], size_t maxBytes) const
{
    if (views == nullptr || maxBytes == 0) {
        return 0;
    }

    size_t count = m_recvBuffer.readableViews(views, maxBytes);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += views[i].len;
    }
    return total;
}

int32_t StreamImpl::consumeRead(size_t bytes)
{
    if (bytes > m_recvBuffer.size()) {
        return StreamErr(UTP_ERR_OVERFLOW);
    }

    m_recvBuffer.consume(bytes);
    maybeNotifyClosed();
    return static_cast<int32_t>(bytes);
}

Stream::State StreamImpl::state() const
{
    if (m_localFinQueued && m_peerFin) {
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
    return !m_recvBuffer.empty();
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

    const int32_t status = m_conn->sendResetStreamFrame(m_streamId, errorCode, m_sendBufferedOffset);
    if (status != UTP_ERR_OK) {
        if (status < 0) {
            return StreamErr(UTP_ERR_INTERNAL_ERROR);
        }
        return StreamErr(static_cast<utp_error_t>(status));
    }

    m_localFinQueued = true;
    m_localFinSent = true;
    m_peerFin = true;
    m_sendQueue.clear();
    m_sendQueuedBytes = 0;
    m_sendBuffer.consume(m_sendBuffer.size());
    m_recvFragments.clear();
    m_recvFragmentsBytes = 0;
    m_recvBuffer.consume(m_recvBuffer.size());
    maybeNotifyClosed();
    return UTP_ERR_OK;
}

bool StreamImpl::resetReceived() const
{
    return m_resetByPeer;
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

int32_t StreamImpl::onFrame(const FrameStream &frame)
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

    if (frameOffset < m_recvOffset) {
        const uint64_t trim = m_recvOffset - frameOffset;
        if (trim >= frameLength) {
            if (STREAM_IS_FIN(frame.stream_flag) && frameOffset + frameLength == m_recvOffset) {
                m_peerFin = true;
                maybeNotifyClosed();
            }
            return UTP_ERR_OK;
        }

        frameOffset += trim;
        frameLength = static_cast<uint16_t>(frameLength - trim);
        data += trim;
    }

    RecvFragment fragment;
    fragment.fin = STREAM_IS_FIN(frame.stream_flag);
    fragment.data.resize(frameLength);
    if (frameLength > 0) {
        std::memcpy(fragment.data.data(), data, frameLength);
    }

    auto existing = m_recvFragments.find(frameOffset);
    if (existing == m_recvFragments.end()) {
        if (m_recvFragmentsBytes + frameLength > kMaxRecvFragmentBytes) {
            return UTP_ERR_WOULD_BLOCK;
        }
        m_recvFragments.emplace(frameOffset, std::move(fragment));
        m_recvFragmentsBytes += frameLength;
    } else if (existing->second.data.size() < frameLength) {
        const size_t oldSize = existing->second.data.size();
        const size_t nextBytes = m_recvFragmentsBytes - oldSize + frameLength;
        if (nextBytes > kMaxRecvFragmentBytes) {
            return UTP_ERR_WOULD_BLOCK;
        }
        existing->second = std::move(fragment);
        m_recvFragmentsBytes = nextBytes;
    } else if (fragment.fin) {
        existing->second.fin = true;
    }

    const size_t beforeSize = m_recvBuffer.size();
    drainRecvFragments();

    if (m_onReadable && m_recvBuffer.size() > beforeSize) {
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

    m_sendQueue.clear();
    m_sendQueuedBytes = 0;
    m_sendBuffer.consume(m_sendBuffer.size());
    m_recvFragments.clear();
    m_recvFragmentsBytes = 0;
    m_recvBuffer.consume(m_recvBuffer.size());
    m_localFinQueued = true;
    m_localFinSent = true;
    m_peerFin = true;

    notifyResetOnce();
    maybeNotifyClosed();
    return UTP_ERR_OK;
}

int32_t StreamImpl::flushPendingSends()
{
    if (m_conn == nullptr) {
        return UTP_ERR_INVALID_STATE;
    }

    while (!m_sendQueue.empty()) {
        PendingSendChunk &chunk = m_sendQueue.front();

        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        bool frameFin = chunk.fin;

        if (chunk.bytes > 0) {
            ConstBufferView views[2];
            const size_t count = m_sendBuffer.readableViews(views, chunk.bytes);
            if (count == 0 || views[0].data == nullptr || views[0].len == 0) {
                return UTP_ERR_INVALID_STATE;
            }

            payload = static_cast<const uint8_t *>(views[0].data);
            payloadLen = std::min<size_t>(std::min<size_t>(chunk.bytes, views[0].len), UINT16_MAX);
            frameFin = chunk.fin && (payloadLen == chunk.bytes);
        }

        const int32_t status = m_conn->sendStreamFrame(m_streamId,
                                                       chunk.offset,
                                                       payload,
                                                       payloadLen,
                                                       frameFin);
        if (status == UTP_ERR_OK) {
            if (payloadLen > 0) {
                m_sendBuffer.consume(payloadLen);
                chunk.offset += payloadLen;
                chunk.bytes -= payloadLen;
                if (m_sendQueuedBytes >= payloadLen) {
                    m_sendQueuedBytes -= payloadLen;
                } else {
                    m_sendQueuedBytes = 0;
                }
            }

            if (frameFin) {
                m_localFinSent = true;
            }

            if (chunk.bytes == 0) {
                m_sendQueue.erase(m_sendQueue.begin());
            }
            continue;
        }

        if (status == UTP_ERR_WOULD_BLOCK) {
            m_conn->scheduleWrite();
        }
        return status;
    }

    return UTP_ERR_OK;
}

int32_t StreamImpl::onConnectionWritable()
{
    const int32_t status = flushPendingSends();
    if (status != UTP_ERR_OK && status != UTP_ERR_WOULD_BLOCK) {
        return status;
    }

    maybeNotifyWritable(true);
    return status;
}

size_t StreamImpl::appWriteCredit() const
{
    if (m_sendQueuedBytes >= kDefaultBufferCapacity) {
        return 0;
    }
    return kDefaultBufferCapacity - m_sendQueuedBytes;
}

void StreamImpl::drainRecvFragments()
{
    while (true) {
        auto it = m_recvFragments.find(m_recvOffset);
        if (it == m_recvFragments.end()) {
            break;
        }

        RecvFragment fragment = std::move(it->second);
        if (m_recvFragmentsBytes >= fragment.data.size()) {
            m_recvFragmentsBytes -= fragment.data.size();
        } else {
            m_recvFragmentsBytes = 0;
        }
        m_recvFragments.erase(it);

        if (!fragment.data.empty()) {
            m_recvBuffer.write(fragment.data.data(), fragment.data.size());
        }

        m_recvOffset += fragment.data.size();
        if (fragment.fin) {
            m_peerFin = true;
        }
    }
}

void StreamImpl::maybeNotifyClosed()
{
    if (m_closedNotified) {
        return;
    }

    if (m_localFinQueued && m_localFinSent && m_peerFin && m_recvBuffer.empty()) {
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
