/*************************************************************************
    > File Name: buffer_stream_utils.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年04月28日 星期日 14时23分15秒
 ************************************************************************/

#ifndef __EULAR_BUFFER_STREAM_UTILS_H__
#define __EULAR_BUFFER_STREAM_UTILS_H__

#include <utils/buffer_stream.h>

namespace eular {

template<typename T, size_t size>
BufferStream &operator<<(BufferStream &stream, const T (&item)[size])
{
    static_assert(!std::is_same<T, nullptr_t>::value, "no support nullptr_t");
    if (std::is_fundamental<T>::value) {
        stream.write(item, sizeof(T) * size);
    } else {
        for (size_t i = 0; i < size; ++i) {
            // 非基础类型由外部进行重载, 自定义输入
            ::operator<<(stream, item[i]);
        }
    }

    return stream;
}

template<typename T, size_t size>
BufferStream &operator>>(BufferStream &stream, T (&item)[size])
{
    static_assert(!std::is_same<T, nullptr_t>::value, "no support nullptr_t");
    if (std::is_fundamental<T>::value) {
        if (!stream.read(item, sizeof(T) * size)) {
            throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
        }
    } else {
        for (size_t i = 0; i < size; ++i) {
            // 非基础类型由外部进行重载, 自定义输出
            ::operator>>(stream, item[i]);
        }
    }

    return stream;
}

template<size_t size>
BufferStream &operator<<(BufferStream &stream, const char (&item)[size])
{
    stream.write(item, size);
    return stream;
}

template<size_t size>
BufferStream &operator>>(BufferStream &stream, char (&buffer)[size])
{
    if (!stream.read(buffer, size)) {
        throw Exception(String8::Format("Read error, maybe insufficient data. [%s:%d]", __FILE__, __LINE__));
    }

    return stream;
}

} // namespace eular

#endif // __EULAR_BUFFER_STREAM_UTILS_H__