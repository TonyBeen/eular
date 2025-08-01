/*************************************************************************
    > File Name: types.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 07 Sep 2022 09:14:53 AM CST
 ************************************************************************/

#ifndef __EULAR_TYPES_H__
#define __EULAR_TYPES_H__

#include <type_traits>  // for enable_if

#include <utils/endian.hpp>

namespace eular {
// 8字节类型转换
template<typename T>
typename std::enable_if<sizeof(T) == sizeof(uint64_t), T>::type
byteswap(T value)
{
    return (T)byteswap_64((uint64_t)value);
}

// 4字节类型转换
template<typename T>
typename std::enable_if<sizeof(T) == sizeof(uint32_t), T>::type
byteswap(T value)
{
    return (T)byteswap_32((uint32_t)value);
}

// 2字节类型转换
template<typename T>
typename std::enable_if<sizeof(T) == sizeof(uint16_t), T>::type
byteswap(T value)
{
    return (T)byteswap_16((uint16_t)value);
}

#if BYTE_ORDER == BIG_ENDIAN
template<typename T>
T toBigEndian(T value)
{
    return value;
}

template<typename T>
T toLittleEndian(T value)
{
    return byteswap(value);
}

#else

// 将value转换为大端字节数，在小端机执行byteswap
template<typename T>
T toBigEndian(T value)
{
    return byteswap(value);
}

// 将value转换为小端字节数，在小端机直接返回
template<typename T>
T toLittleEndian(T value)
{
    return value;
}
#endif


template<typename T>
struct little_endian
{
    little_endian() = default;
    little_endian(const T &t) { m_data = toLittleEndian(t); }
    little_endian(const little_endian &other) { m_data = other.m_data; }
    little_endian &operator=(const little_endian &other) {
        if (&other != this) {
            m_data = other.m_data;
        }
        return *this;
    }

    T operator=(const T &t) { m_data = toLittleEndian(t); return t; }

    operator T() const {
    #if BYTE_ORDER == LITTLE_ENDIAN
        return m_data;
    #else
        return toBigEndian(m_data);
    #endif
    }

private:
    T   m_data{};
};

typedef little_endian<int16_t>  int16_le_t;
typedef little_endian<int32_t>  int32_le_t;
typedef little_endian<uint16_t> uint16_le_t;
typedef little_endian<uint32_t> uint32_le_t;
typedef little_endian<int64_t>  int64_le_t;
typedef little_endian<uint64_t> uint64_le_t;
typedef little_endian<float>    float32_le_t;
typedef little_endian<double>   float64_le_t;

} // namespace eular

#endif // __EULAR_TYPES_H__