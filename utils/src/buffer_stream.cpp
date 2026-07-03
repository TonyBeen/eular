/*************************************************************************
    > File Name: buffer_stream.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年04月25日 星期四 08时58分22秒
 ************************************************************************/

#include "utils/buffer_stream.h"
#include "utils/utils.h"

#include <limits>

namespace eular {
BufferStream::BufferStream() :
    m_buffer(nullptr),
    m_rpos(0),
    m_wpos(0)
{
}

BufferStream::BufferStream(ByteBuffer &buffer) :
    m_buffer(std::addressof(buffer)),
    m_rpos(0),
    m_wpos(buffer.size())
{
}

BufferStream::BufferStream(BufferStream &&other) :
    m_buffer(other.m_buffer),
    m_rpos(other.m_rpos),
    m_wpos(other.m_wpos)
{
    other.m_buffer = nullptr;
    other.m_rpos = 0;
    other.m_wpos = 0;
}

BufferStream::~BufferStream()
{
}

BufferStream &BufferStream::operator=(BufferStream &&other)
{
    if (this != std::addressof(other)) {
        std::swap(m_buffer, other.m_buffer);
        std::swap(m_rpos, other.m_rpos);
        std::swap(m_wpos, other.m_wpos);
    }

    return *this;
}

BufferStream &BufferStream::operator<<(bool item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(int8_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(uint8_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(wchar_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(int16_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(uint16_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(float item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(int32_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(uint32_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(double item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(int64_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(uint64_t item)
{
    write(&item, sizeof(item));
    return *this;
}

BufferStream &BufferStream::operator<<(const std::string &item)
{
    if (item.length() >= std::numeric_limits<uint32_t>::max()) {
        throw Exception(String8::Format("String too large. [%s:%d]", __FILE__, __LINE__));
    }

    // 将\0保存到缓存中
    write(item.c_str(), static_cast<uint32_t>(item.length() + 1));
    return *this;
}

BufferStream &BufferStream::operator>>(bool &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(int8_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(uint8_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(wchar_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(int16_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(uint16_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(float &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(int32_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(uint32_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(double &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(int64_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(uint64_t &item)
{
    if (!read(&item, sizeof(item))) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return *this;
}

BufferStream &BufferStream::operator>>(std::string &item)
{
    checkBuffer();
    std::string temp;
    char ch = 1;
    do {
        if (!read(&ch, sizeof(ch))) {
            throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
        }
        if (ch != '\0') {
            temp.push_back(ch);
        }
    } while (ch != '\0');

    item.assign(temp);

    return *this;
}

void BufferStream::write(const void *data, uint32_t size)
{
    checkBuffer();
    if (size == 0) {
        return;
    }
    if (data == nullptr) {
        throw Exception(String8::Format("Invalid write data. [%s:%d]", __FILE__, __LINE__));
    }

    uint32_t cap = m_buffer->capacity();
    if (m_rpos > m_wpos || m_wpos > m_buffer->size() || m_buffer->size() > cap) {
        throw Exception(String8::Format("Invalid stream state. [%s:%d]", __FILE__, __LINE__));
    }

    if (m_rpos != 0 && size > cap - m_wpos) {
        // 当前读位置不在起点, 且已无法写入这么多字节数, 移除旧的数据
        uint8_t *pBegin = m_buffer->data() + m_rpos;
        uint32_t copySize = m_wpos - m_rpos;

        uint32_t realCopySize = m_buffer->set(pBegin, copySize);
        if (eular_unlikely(realCopySize != copySize)) {
            throw Exception(String8::Format("Not enough memory. [%s:%d]", __FILE__, __LINE__));
        }

        m_rpos = 0;
        m_wpos = copySize;
    }

    if (size > std::numeric_limits<uint32_t>::max() - m_wpos) {
        throw Exception(String8::Format("BufferStream size overflow. [%s:%d]", __FILE__, __LINE__));
    }

    uint32_t expectedSize = m_wpos + size;
    m_buffer->append(static_cast<const uint8_t *>(data), size);
    if (m_buffer->size() != expectedSize) {
        throw Exception(String8::Format("Not enough memory. [%s:%d]", __FILE__, __LINE__));
    }
    m_wpos = expectedSize;
}

bool BufferStream::read(void *data, uint32_t size)
{
    checkBuffer();
    if (size == 0) {
        return true;
    }
    if (data == nullptr || m_rpos > m_wpos) {
        return false;
    }

    if (size > m_wpos - m_rpos) {
        return false;
    }

    // 有足够多的数据可读
    memcpy(data, m_buffer->const_data() + m_rpos, size);
    m_rpos += size;
    return true;
}

void BufferStream::checkBuffer() const
{
    if (m_buffer == nullptr) {
        throw Exception("Invalid call");
    }
}

} // namespace eular
