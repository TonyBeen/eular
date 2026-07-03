/*************************************************************************
    > File Name: literals.hpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 02:47:01 PM CST
 ************************************************************************/

#ifndef __UTILS_LITERALS_HPP__
#define __UTILS_LITERALS_HPP__

#include <stdio.h>
#include <stdint.h>

namespace eular {
namespace literals {
    using ull = unsigned long long;
    using ll = long long;
    constexpr size_t operator "" _KB(ull value) {
        return static_cast<size_t>(value * 1024);
    }
    constexpr size_t operator "" _MB(ull value) {
        return static_cast<size_t>(value * 1024 * 1024);
    }
    constexpr size_t operator "" _GB(ull value) {
        return static_cast<size_t>(value * 1024 * 1024 * 1024);
    }
    constexpr size_t operator "" _TB(ull value) {
        return static_cast<size_t>(value * 1024 * 1024 * 1024 * 1024);
    }

    constexpr size_t operator "" _s(ull value) {
        return static_cast<size_t>(value * 1000);
    }
    constexpr size_t operator "" _ms(ull value) {
        return static_cast<size_t>(value);
    }
    constexpr size_t operator "" _us(ull value) {
        return static_cast<size_t>(value / 1000);
    }
} // namespace literals
} // namespace eular

#endif // __UTILS_LITERALS_HPP__
