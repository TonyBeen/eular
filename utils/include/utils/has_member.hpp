/*************************************************************************
    > File Name: has_member.hpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 04 Jul 2024 04:00:14 PM CST
 ************************************************************************/

#ifndef __EULAR_UTILS_HAS_MEMBER_H__
#define __EULAR_UTILS_HAS_MEMBER_H__

#include <type_traits>
#include <utility>

#define HAS_MEMBER(NameSpace, Func)                                                                         \
namespace NameSpace {                                                                                       \
template<typename T, typename R, typename... Args>                                                          \
struct has_member_##Func                                                                                    \
{                                                                                                           \
private:                                                                                                    \
    template<typename U>                                                                                    \
    static auto Check(int) -> decltype(std::declval<U>().Func(std::declval<Args>()...), std::true_type());  \
                                                                                                            \
    template<typename U>                                                                                    \
    static std::false_type Check(...);                                                                      \
                                                                                                            \
    template<typename U>                                                                                    \
    static auto GetReturnType(int) -> decltype(std::declval<U>().Func(std::declval<Args>()...)) {}          \
                                                                                                            \
    template<typename U>                                                                                    \
    static void GetReturnType(...) {}                                                                       \
                                                                                                            \
public:                                                                                                     \
    static constexpr auto value = decltype(Check<T>(0))::value &&                                           \
                                  std::is_same<R, decltype(GetReturnType<T>(0))>::value;                    \
};                                                                                                          \
}

#define HAS_MEMBER_CONST(NameSpace, Func)                                                                   \
namespace NameSpace {                                                                                       \
template<typename T, typename R, typename... Args>                                                          \
struct has_member_##Func                                                                                    \
{                                                                                                           \
private:                                                                                                    \
    template<typename U>                                                                                    \
    static auto Check(int) -> decltype(std::declval<const U>().Func(std::declval<Args>()...), std::true_type());  \
                                                                                                            \
    template<typename U>                                                                                    \
    static std::false_type Check(...);                                                                      \
                                                                                                            \
    template<typename U>                                                                                    \
    static auto GetReturnType(int) -> decltype(std::declval<U>().Func(std::declval<Args>()...)) {}          \
                                                                                                            \
    template<typename U>                                                                                    \
    static void GetReturnType(...) {}                                                                       \
                                                                                                            \
public:                                                                                                     \
    static constexpr auto value = decltype(Check<T>(0))::value &&                                           \
                                  std::is_same<R, decltype(GetReturnType<T>(0))>::value;                    \
};                                                                                                          \
}

#endif // __EULAR_UTILS_HAS_MEMBER_H__
