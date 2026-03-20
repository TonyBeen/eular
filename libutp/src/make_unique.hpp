/*************************************************************************
    > File Name: make_unique.hpp
    > Author: eular
    > Brief:
    > Created Time: Mon 09 Feb 2026 05:15:23 PM CST
 ************************************************************************/

#ifndef __UTP_MAKE_UNIQUE_HPP__
#define __UTP_MAKE_UNIQUE_HPP__

#include <memory>

#if __cplusplus < 201402L

namespace std {
namespace detail {
// 1) 非数组
template <class T>
struct unique_if {
    using SingleObject = std::unique_ptr<T>;
};

// 2) 动态长度数组 T[]
template <class T>
struct unique_if<T[]> {
    using ArrayObject = std::unique_ptr<T[]>;
};

// 3) 定长数组 T[N]：禁用
template <class T, std::size_t N>
struct unique_if<T[N]> {
    using Unsupported = void;
};

} // namespace detail

// 非数组
template <class T, class... Args>
typename detail::unique_if<T>::SingleObject make_unique(Args&&... args) {
    static_assert(!std::is_array<T>::value, "T must not be an array in this overload");
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// 动态长度数组
template <class T>
typename detail::unique_if<T>::ArrayObject make_unique(std::size_t n) {
    using U = typename std::remove_extent<T>::type;
    return std::unique_ptr<T>(new U[n]());
}

// 定长数组：删除
template <class T, class... Args>
typename detail::unique_if<T>::Unsupported make_unique(Args&&...) = delete;
} // namespace std

#endif // __cplusplus < 201402L

#endif // __UTP_MAKE_UNIQUE_HPP__
