/*************************************************************************
    > File Name: ring_buffer.h
    > Author: eular
    > Brief:
 ************************************************************************/

#ifndef __UTP_UTIL_RING_BUFFER_H__
#define __UTP_UTIL_RING_BUFFER_H__

#include <cstddef>
#include <cstdint>
#include <memory>

#include "utp/stream.h"

namespace eular {
namespace utp {

class RingBuffer
{
public:
    RingBuffer() = default;
    explicit RingBuffer(size_t capacity);

    void    ensureFree(size_t freeBytes);
    size_t  size() const { return m_size; }
    size_t  capacity() const { return m_capacity; }
    size_t  freeSize() const { return capacity() - m_size; }
    bool    empty() const { return m_size == 0; }

    size_t  readableViews(Stream::ConstBufferView views[2], size_t maxBytes) const;
    size_t  readableViewsFrom(Stream::ConstBufferView views[2], size_t startOffset, size_t maxBytes) const;
    size_t  writableViews(Stream::MutableBufferView views[2], size_t maxBytes);
    void    produce(size_t bytes);
    void    consume(size_t bytes);
    size_t  write(const uint8_t *data, size_t len);
    size_t  read(uint8_t *buffer, size_t len);

private:
    std::unique_ptr<uint8_t[]> m_buffer;
    size_t m_capacity{0};
    size_t m_head{0};
    size_t m_size{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_RING_BUFFER_H__
