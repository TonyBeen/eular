/*************************************************************************
    > File Name: ring_buffer.cpp
    > Author: hsz
    > Brief:
    > Created Time: Wed 24 Dec 2025 03:51:55 PM CST
 ************************************************************************/

#include "utils/ring_buffer.h"

namespace eular {

RingBuffer::RingBuffer(size_t capacity) :
    m_capacity(capacity),
    m_buffer(capacity),
    m_readPos(0),
    m_writePos(0),
    m_usedSize(0),
    m_totalBytesWritten(0),
    m_totalBytesRead(0)
{
    if (capacity == 0) {
        throw std::invalid_argument("Capacity must be greater than 0");
    }
}

size_t RingBuffer::write(const void* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return 0;
    }

    size_t writable = writableSize();
    size_t to_write = std:: min(len, writable);

    if (to_write == 0) {
        return 0;
    }

    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t first_part = std::min(to_write, m_capacity - m_writePos);
    
    std::memcpy(m_buffer. data() + m_writePos, src, first_part);
    
    if (to_write > first_part) {
        std::memcpy(m_buffer.data(), src + first_part, to_write - first_part);
    }

    m_writePos = (m_writePos + to_write) % m_capacity;
    m_usedSize += to_write;
    m_totalBytesWritten += to_write;

    return to_write;
}

size_t RingBuffer::read(void* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return 0;
    }

    size_t readable = readableSize();
    size_t to_read = std::min(len, readable);

    if (to_read == 0) {
        return 0;
    }

    uint8_t* dst = static_cast<uint8_t*>(data);
    size_t first_part = std:: min(to_read, m_capacity - m_readPos);
    
    std:: memcpy(dst, m_buffer.data() + m_readPos, first_part);

    if (to_read > first_part) {
        std::memcpy(dst + first_part, m_buffer. data(), to_read - first_part);
    }

    m_readPos = (m_readPos + to_read) % m_capacity;
    m_usedSize -= to_read;
    m_totalBytesRead += to_read;

    return to_read;
}

size_t RingBuffer::peek(void* data, size_t len) const
{
    if (data == nullptr || len == 0) {
        return 0;
    }

    size_t readable = readableSize();
    size_t to_peek = std::min(len, readable);

    if (to_peek == 0) {
        return 0;
    }

    uint8_t* dst = static_cast<uint8_t*>(data);
    size_t temp_read_pos = m_readPos;

    size_t first_part = std::min(to_peek, m_capacity - temp_read_pos);
    std::memcpy(dst, m_buffer.data() + temp_read_pos, first_part);

    if (to_peek > first_part) {
        std::memcpy(dst + first_part, m_buffer.data(), to_peek - first_part);
    }

    return to_peek;
}

size_t RingBuffer::peekAt(void* data, size_t len, size_t offset) const
{
    if (data == nullptr || len == 0 || offset >= m_usedSize) {
        return 0;
    }

    size_t available = m_usedSize - offset;
    size_t to_peek = std::min(len, available);

    if (to_peek == 0) {
        return 0;
    }

    uint8_t* dst = static_cast<uint8_t*>(data);
    size_t temp_read_pos = (m_readPos + offset) % m_capacity;

    size_t first_part = std::min(to_peek, m_capacity - temp_read_pos);
    std::memcpy(dst, m_buffer.data() + temp_read_pos, first_part);

    if (to_peek > first_part) {
        std::memcpy(dst + first_part, m_buffer. data(), to_peek - first_part);
    }

    return to_peek;
}

size_t RingBuffer:: skip(size_t len)
{
    size_t readable = readableSize();
    size_t to_skip = std::min(len, readable);

    m_readPos = (m_readPos + to_skip) % m_capacity;
    m_usedSize -= to_skip;
    m_totalBytesRead += to_skip;

    return to_skip;
}

void RingBuffer::clear()
{
    m_readPos = 0;
    m_writePos = 0;
    m_usedSize = 0;
}

void RingBuffer::reset()
{
    clear();
    m_totalBytesWritten = 0;
    m_totalBytesRead = 0;
}

bool RingBuffer::getReadableRegion(const uint8_t*& ptr, size_t& len) const
{
    if (m_usedSize == 0) {
        ptr = nullptr;
        len = 0;
        return false;
    }

    ptr = m_buffer.data() + m_readPos;
    if (m_readPos + m_usedSize <= m_capacity) {
        len = m_usedSize;
        return false;
    } else {
        len = m_capacity - m_readPos;
        return true;
    }
}

bool RingBuffer::getWritableRegion(uint8_t*& ptr, size_t& len)
{
    size_t writable = writableSize();

    if (writable == 0) {
        ptr = nullptr;
        len = 0;
        return false;
    }

    ptr = m_buffer.data() + m_writePos;
    if (m_writePos + writable <= m_capacity) {
        len = writable;
        return false;
    } else {
        len = m_capacity - m_writePos;
        return true;
    }
}

void RingBuffer::commitWrite(size_t len)
{
    size_t writable = writableSize();
    len = std::min(len, writable);

    m_writePos = (m_writePos + len) % m_capacity;
    m_usedSize += len;
    m_totalBytesWritten += len;
}

void RingBuffer::commitRead(size_t len) {
    len = std::min(len, m_usedSize);

    m_readPos = (m_readPos + len) % m_capacity;
    m_usedSize -= len;
    m_totalBytesRead += len;
}
}