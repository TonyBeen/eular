/*************************************************************************
    > File Name: ring_buffer.cpp
    > Author: eular
    > Brief:
 ************************************************************************/

#include "util/ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace eular {
namespace utp {

RingBuffer::RingBuffer(size_t capacity) :
    m_buffer(new uint8_t[std::max<size_t>(capacity, 1)]),
    m_capacity(std::max<size_t>(capacity, 1))
{
}

void RingBuffer::ensureFree(size_t freeBytes)
{
    if (freeSize() >= freeBytes) {
        return;
    }

    const size_t required = m_size + freeBytes;
    size_t newCap = std::max<std::size_t>(m_capacity, 1);
    while (newCap < required) {
        newCap *= 2;
    }

    std::unique_ptr<uint8_t[]> newBuffer(new uint8_t[newCap]);
    Stream::ConstBufferView views[2];
    const size_t count = readableViews(views, m_size);
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
        if (views[i].data != nullptr && views[i].len > 0) {
            std::memcpy(newBuffer.get() + copied, views[i].data, views[i].len);
            copied += views[i].len;
        }
    }

    m_buffer.swap(newBuffer);
    m_capacity = newCap;
    m_head = 0;
}

size_t RingBuffer::readableViews(Stream::ConstBufferView views[2], size_t maxBytes) const
{
    if (views == nullptr) {
        return 0;
    }

    views[0] = {};
    views[1] = {};
    if (m_capacity == 0 || m_size == 0 || maxBytes == 0) {
        return 0;
    }

    const size_t bytes = std::min(maxBytes, m_size);
    const size_t first = std::min(bytes, m_capacity - m_head);
    views[0].data = m_buffer.get() + m_head;
    views[0].len = first;

    const size_t second = bytes - first;
    if (second > 0) {
        views[1].data = m_buffer.get();
        views[1].len = second;
        return 2;
    }

    return 1;
}

size_t RingBuffer::readableViewsFrom(Stream::ConstBufferView views[2], size_t startOffset, size_t maxBytes) const
{
    if (views == nullptr) {
        return 0;
    }

    views[0] = {};
    views[1] = {};
    if (m_capacity == 0 || m_size == 0 || maxBytes == 0 || startOffset >= m_size) {
        return 0;
    }

    const size_t available = m_size - startOffset;
    const size_t bytes = std::min(maxBytes, available);
    const size_t start = (m_head + startOffset) % m_capacity;
    const size_t first = std::min(bytes, m_capacity - start);
    views[0].data = m_buffer.get() + start;
    views[0].len = first;

    const size_t second = bytes - first;
    if (second > 0) {
        views[1].data = m_buffer.get();
        views[1].len = second;
        return 2;
    }

    return 1;
}

size_t RingBuffer::writableViews(Stream::MutableBufferView views[2], size_t maxBytes)
{
    if (views == nullptr) {
        return 0;
    }

    views[0] = {};
    views[1] = {};
    if (m_capacity == 0 || maxBytes == 0 || freeSize() == 0) {
        return 0;
    }

    const size_t bytes = std::min(maxBytes, freeSize());
    const size_t tail = (m_head + m_size) % m_capacity;
    const size_t first = std::min(bytes, m_capacity - tail);
    views[0].data = m_buffer.get() + tail;
    views[0].len = first;

    const size_t second = bytes - first;
    if (second > 0) {
        views[1].data = m_buffer.get();
        views[1].len = second;
        return 2;
    }

    return 1;
}

void RingBuffer::produce(size_t bytes)
{
    m_size = std::min(m_size + bytes, m_capacity);
}

void RingBuffer::consume(size_t bytes)
{
    const size_t n = std::min(bytes, m_size);
    if (m_capacity != 0) {
        m_head = (m_head + n) % m_capacity;
    }
    m_size -= n;
}

size_t RingBuffer::write(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    ensureFree(len);
    Stream::MutableBufferView views[2];
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

size_t RingBuffer::read(uint8_t *buffer, size_t len)
{
    if (buffer == nullptr || len == 0) {
        return 0;
    }

    Stream::ConstBufferView views[2];
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

} // namespace utp
} // namespace eular
