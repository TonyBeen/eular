/*************************************************************************
    > File Name: ring_buffer.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 24 Dec 2025 03:51:45 PM CST
 ************************************************************************/

#ifndef __RING_m_bufferH__
#define __RING_m_bufferH__

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <vector>

#include <utils/sysdef.h>

namespace eular {

class UTILS_API RingBuffer {
public: 
    explicit RingBuffer(size_t capacity);

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = default;
    RingBuffer& operator=(RingBuffer&&) = default;
    ~RingBuffer() = default;

    // 基本读写
    size_t write(const void* data, size_t len);
    size_t read(void* data, size_t len);
    size_t peek(void* data, size_t len) const;
    size_t peekAt(void* data, size_t len, size_t offset) const;
    size_t skip(size_t len);

    // 清空操作
    void clear();
    void reset();

    // 容量查询
    size_t capacity() const { return m_capacity; }
    size_t readableSize() const { return m_usedSize; }
    size_t writableSize() const { return m_capacity - m_usedSize; }
    bool isEmpty() const { return m_usedSize == 0; }
    bool isFull() const { return m_usedSize == m_capacity; }

    // 累计统计
    uint64_t totalBytesWritten() const { return m_totalBytesWritten; }
    uint64_t totalBytesRead() const { return m_totalBytesRead; }
    uint64_t writeOffset() const { return m_totalBytesWritten; }
    uint64_t readOffset() const { return m_totalBytesRead; }
    uint64_t pendingDataOffset() const { return m_totalBytesRead; }

    // 零拷贝
    bool getReadableRegion(const uint8_t *&ptr, size_t& len) const;
    bool getWritableRegion(uint8_t*& ptr, size_t& len);
    void commitWrite(size_t len);
    void commitRead(size_t len);

private:
    size_t      m_capacity;
    std::vector<uint8_t> m_buffer;
    size_t      m_readPos;
    size_t      m_writePos;
    size_t      m_usedSize;
    uint64_t    m_totalBytesWritten;
    uint64_t    m_totalBytesRead;
};

} // namespace eular

#endif // __RING_m_bufferH__
