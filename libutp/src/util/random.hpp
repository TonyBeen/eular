/*************************************************************************
    > File Name: random.hpp
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 09:42:08 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_H__
#define __UTP_UTIL_H__

#include <random>
#include <limits>
#include <type_traits>

namespace eular {
namespace utp {
inline std::mt19937_64 &rng()
{
    static thread_local std::mt19937_64 gen((std::random_device())());
    return gen;
}

// 浮点范围 [lo, hi)；当 lo > hi 时会自动交换
template <typename Float, typename = typename std::enable_if<std::is_floating_point<Float>::value, Float>::type>
inline Float Random(Float lo, Float hi)
{
    if (lo > hi) {
        Float t = lo;
        lo = hi;
        hi = t;
    }

    // generate_canonical 的第二参数使用浮点类型的有效位数以获得最佳精度
    return std::generate_canonical<Float, std::numeric_limits<Float>::digits>(rng()) * (hi - lo) + lo;
}

// 整数范围 [lo, hi]
template <typename Int, typename = typename std::enable_if<std::is_integral<Int>::value, Int>::type>
inline Int Random(Int lo, Int hi)
{
    if (lo > hi) {
        Int t = lo;
        lo = hi;
        hi = t;
    }

    std::uniform_int_distribution<Int> d(lo, hi);
    return d(rng());
}

// 填充缓冲区（字节）
inline void RandomBytes(void* data, std::size_t n)
{
    auto* p = static_cast<unsigned char*>(data);
    std::uniform_int_distribution<int> d(0, 255);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(d(rng()));
}
} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_H__
