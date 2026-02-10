/*************************************************************************
    > File Name: serialize.h
    > Author: eular
    > Brief:
    > Created Time: Wed 04 Feb 2026 10:08:09 AM CST
 ************************************************************************/

#ifndef __UTIL_SERIALIZE_HPP__
#define __UTIL_SERIALIZE_HPP__

#include <cstdint>
#include <cstring>
#include <type_traits>

#include <utils/endian.hpp>

namespace eular {
class Serialize
{
    template <size_t N> struct UintSelector;

    template <typename R, typename T>
    static void SerializeImpl(R* buf, T value, std::integral_constant<size_t, 1>) {
        std::memcpy(buf, &value, 1);
    }

    template <typename R, typename T>
    static void SerializeImpl(R* buf, T value, std::integral_constant<size_t, 2>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT beNumber = htobe16(static_cast<UintT>(value));
        std::memcpy(buf, &beNumber, sizeof(UintT));
    }

    template <typename R, typename T>
    static void SerializeImpl(R* buf, T value, std::integral_constant<size_t, 4>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT bits;
        std::memcpy(&bits, &value, sizeof(T)); // 兼容 float 类型
        UintT beNumber = htobe32(bits);
        std::memcpy(buf, &beNumber, sizeof(UintT));
    }

    template <typename R, typename T>
    static void SerializeImpl(R* buf, T value, std::integral_constant<size_t, 8>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT bits;
        std::memcpy(&bits, &value, sizeof(T)); // 兼容 double 类型
        UintT beNumber = htobe64(bits);
        std::memcpy(buf, &beNumber, sizeof(UintT));
    }

    template <typename R, typename T>
    static void DeserializeImpl(const R* buf, T &value, std::integral_constant<size_t, 1>) {
        uint8_t tmp = 0;
        std::memcpy(&tmp, buf, 1);
        value = static_cast<T>(tmp);
    }

    template <typename R, typename T>
    static void DeserializeImpl(const R* buf, T &value, std::integral_constant<size_t, 2>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT beNumber = 0;
        std::memcpy(&beNumber, buf, sizeof(UintT));
        UintT host = be16toh(beNumber);
        value = static_cast<T>(host);
    }

    template <typename R, typename T>
    static void DeserializeImpl(const R* buf, T &value, std::integral_constant<size_t, 4>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT beNumber = 0;
        std::memcpy(&beNumber, buf, sizeof(UintT));
        UintT host = be32toh(beNumber);
        std::memcpy(&value, &host, sizeof(host)); // 兼容 float 类型
    }

    template <typename R, typename T>
    static void DeserializeImpl(const R* buf, T &value, std::integral_constant<size_t, 8>) {
        using UintT = typename UintSelector<sizeof(T)>::type;
        UintT beNumber = 0;
        std::memcpy(&beNumber, buf, sizeof(UintT));
        UintT host = be64toh(beNumber);
        std::memcpy(&value, &host, sizeof(host)); // 兼容 double 类型
    }

    template <typename T>
    struct SupportedSize {
        enum { value = (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) };
    };

public:
    Serialize() = default;
    ~Serialize() = default;

    /**
     * @brief 将 value 序列化到 buf 中 (大端序)
     *
     * @tparam R 返回值类型, 必须为字节类型（如 uint8_t）
     * @tparam T 要序列化的值类型, 必须为算术类型
     * @param[in] buf 缓存区指针
     * @param[in,out] size 可用字节数引用, 写入成功后会减少相应字节数
     * @param value 要序列化的值
     * @return R* 成功返回下一个写入位置, 失败返回 nullptr
     */
    template <typename R, typename T>
    static typename std::enable_if<std::is_arithmetic<T>::value, R*>::type
    SerializeTo(R *buf, size_t &size, T value)
    {
        static_assert(sizeof(R) == 1, "Buffer must be byte-sized");
        static_assert(SupportedSize<T>::value, "Only 1/2/4/8-byte types supported");

        if (buf == nullptr || size < sizeof(T)) {
            return nullptr;
        }

        SerializeImpl(buf, value, std::integral_constant<size_t, sizeof(T)>{});
        size -= sizeof(T);
        return buf + sizeof(T);
    }

    // 枚举类型
    template <typename R, typename T>
    static typename std::enable_if<std::is_enum<T>::value, R*>::type
    SerializeTo(R *buf, size_t &size, T value)
    {
        static_assert(sizeof(R) == 1, "Buffer must be byte-sized");
        static_assert(SupportedSize<T>::value, "Only 1/2/4/8-byte types supported");

        typedef typename std::underlying_type<T>::type U;
        // NOTE 枚举大小应该等于 underlying 大小
        static_assert(sizeof(T) == sizeof(U), "Enum size must match underlying type size");

        U u = static_cast<U>(value);
        return SerializeTo<R, U>(buf, size, u);
    }

    /**
     * @brief 从 buf 中反序列化出 value (大端序)
     *
     * @tparam R 返回值类型, 必须为字节类型（如 uint8_t）
     * @tparam T 要反序列化的值类型, 必须为算术类型
     * @param buf 缓存区指针
     * @param[in,out] size 剩余可读字节数引用, 读取成功后会减少相应字节数
     * @param[out] value 反序列化得到的值引用
     * @return const R* 成功返回下一个读取位置, 失败返回 nullptr
     */
    template <typename R, typename T>
    static typename std::enable_if<std::is_arithmetic<T>::value, const R*>::type
    DeserializeFrom(const R *buf, size_t &size, T &value)
    {
        static_assert(sizeof(R) == 1, "Buffer must be byte-sized");
        static_assert(SupportedSize<T>::value, "Only 1/2/4/8-byte types supported");

        if (buf == nullptr) return nullptr;
        if (size < sizeof(T)) {
            return nullptr;
        }

        DeserializeImpl(buf, value, std::integral_constant<size_t, sizeof(T)>{});
        size -= sizeof(T);
        return buf + sizeof(T);
    }

    // 枚举类型
    template <typename R, typename T>
    static typename std::enable_if<std::is_enum<T>::value, const R*>::type
    DeserializeFrom(const R *buf, size_t &size, T &value)
    {
        static_assert(sizeof(R) == 1, "Buffer must be byte-sized");
        static_assert(SupportedSize<T>::value, "Only 1/2/4/8-byte types supported");

        typedef typename std::underlying_type<T>::type U;
        static_assert(sizeof(T) == sizeof(U), "Enum size must match underlying type size");

        U u = 0;
        const R* next = DeserializeFrom<R, U>(buf, size, u);
        if (next == nullptr) {
            return nullptr;
        }

        value = static_cast<T>(u);
        return next;
    }
};

// 模板特化定义 (必须放在类外)
template <> struct Serialize::UintSelector<2> { using type = uint16_t; };
template <> struct Serialize::UintSelector<4> { using type = uint32_t; };
template <> struct Serialize::UintSelector<8> { using type = uint64_t; };

} // namespace eular

#endif // __UTIL_SERIALIZE_HPP__
