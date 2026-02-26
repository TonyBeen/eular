/*************************************************************************
    > File Name: enum_util.hpp
    > Author: eular
    > Brief:
    > Created Time: Thu 26 Feb 2026 11:24:49 AM CST
 ************************************************************************/

#ifndef __UTILS_ENUM_UTIL_HPP__
#define __UTILS_ENUM_UTIL_HPP__

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>

namespace enum_util {
    template <typename E>
    inline typename std::underlying_type<E>::type to_underlying(E e) noexcept {
        return static_cast<typename std::underlying_type<E>::type>(e);
    }

    template <typename E>
    struct enable_bitmask_operators : std::false_type {};

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E>::type
    operator|(E a, E b) noexcept
    {
        return static_cast<E>(to_underlying(a) | to_underlying(b));
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E>::type
    operator&(E a, E b) noexcept
    {
        return static_cast<E>(to_underlying(a) & to_underlying(b));
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E>::type
    operator^(E a, E b) noexcept
    {
        return static_cast<E>(to_underlying(a) ^ to_underlying(b));
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E>::type
    operator~(E a) noexcept
    {
        return static_cast<E>(~to_underlying(a));
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E &>::type
    operator|=(E &a, E b) noexcept
    {
        a = (a | b);
        return a;
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E &>::type
    operator&=(E &a, E b) noexcept
    {
        a = (a & b);
        return a;
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, E &>::type
    operator^=(E &a, E b) noexcept
    {
        a = (a ^ b);
        return a;
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, bool>::type
    has(E value, E flag) noexcept
    {
        return (to_underlying(value & flag) != 0);
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, void>::type
    set(E &value, E flag) noexcept
    {
        value |= flag;
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, void>::type
    clear(E &value, E flag) noexcept
    {
        value &= ~flag;
    }

    inline char ascii_tolower(char c) noexcept
    {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return c;
    }

    inline bool iequals_ascii(const char* a, const char* b) noexcept
    {
        if (!a || !b) {
            return false;
        }

        while (*a && *b) {
            const char ca = ascii_tolower(*a);
            const char cb = ascii_tolower(*b);
            if (ca != cb) {
                return false;
            }
            ++a;
            ++b;
        }

        return *a == *b;
    }

    template <typename E>
    struct enum_entry {
        const char *name;
        E value;
    };

    template <typename E>
    struct enum_traits;

    template <typename E>
    inline const char *to_string(E v)
    {
        const enum_entry<E> *es = enum_traits<E>::entries();
        const std::size_t n = enum_traits<E>::size();
        for (std::size_t i = 0; i < n; ++i) {
            if (es[i].value == v) {
                return es[i].name;
            }
        }
        return nullptr;
    }

    template <typename E>
    inline bool from_string(const char *s, E &out)
    {
        if (!s) {
            return false;
        }

        const enum_entry<E> *es = enum_traits<E>::entries();
        const std::size_t n = enum_traits<E>::size();

        for (std::size_t i = 0; i < n; ++i) {
            if (std::strcmp(es[i].name, s) == 0) {
                out = es[i].value;
                return true;
            }
        }
        return false;
    }

    template <typename E>
    inline bool from_string_ci(const char* s, E& out)
    {
        if (!s) {
            return false;
        }

        const enum_entry<E>* es = enum_traits<E>::entries();
        const std::size_t n = enum_traits<E>::size();

        for (std::size_t i = 0; i < n; ++i) {
            if (iequals_ascii(es[i].name, s))
            {
                out = es[i].value;
                return true;
            }
        }
        return false;
    }

    template <typename E>
    inline bool from_string_ci(const std::string& s, E& out) {
        return from_string_ci<E>(s.c_str(), out);
    }

    template <typename E>
    inline bool from_string(const std::string &s, E &out) {
        return from_string<E>(s.c_str(), out);
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, std::string>::type
    flags_to_string(E v, char sep = '|')
    {
        typedef typename std::underlying_type<E>::type U;

        const enum_entry<E> *es = enum_traits<E>::entries();
        const std::size_t n = enum_traits<E>::size();

        if (static_cast<U>(v) == 0) {
            // 如果注册表里有 value==0 的项（如 None），优先返回其名字
            for (std::size_t i = 0; i < n; ++i) {
                if (static_cast<U>(es[i].value) == 0) {
                    return es[i].name;
                }
            }
            return "0";
        }

        std::ostringstream oss;
        bool first = true;

        for (std::size_t i = 0; i < n; ++i)
        {
            const U uv = static_cast<U>(es[i].value);
            if (uv == 0) {
                continue;
            }

            // 单bit判断：uv & (uv - 1) == 0
            if ((uv & (uv - 1)) == 0 && has(v, es[i].value)) {
                if (!first) {
                    oss << sep;
                }
                oss << es[i].name;
                first = false;
            }
        }

        return oss.str();
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, bool>::type
    flags_from_string(const std::string &s, E &out, char sep = '|')
    {
        out = static_cast<E>(0);

        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t pos = s.find(sep, start);
            std::string token = (pos == std::string::npos)
                                    ? s.substr(start)
                                    : s.substr(start, pos - start);

            // trim 空白
            while (!token.empty() && (token[0] == ' ' || token[0] == '\t')) {
                token.erase(token.begin());
            }
            while (!token.empty() && (token[token.size() - 1] == ' ' || token[token.size() - 1] == '\t')) {
                token.erase(token.end() - 1);
            }

            if (!token.empty()) {
                E one;
                if (!from_string<E>(token.c_str(), one)) {
                    return false;
                }
                out |= one;
            }

            if (pos == std::string::npos) {
                break;
            }
            start = pos + 1;
        }

        return true;
    }

    template <typename E>
    typename std::enable_if<enable_bitmask_operators<E>::value, bool>::type
    flags_from_string_ci(const std::string& s, E& out, char sep = '|')
    {
        out = static_cast<E>(0);

        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t pos = s.find(sep, start);
            std::string token = (pos == std::string::npos)
                ? s.substr(start)
                : s.substr(start, pos - start);

            while (!token.empty() && (token[0] == ' ' || token[0] == '\t')) {
                token.erase(token.begin());
            }
            while (!token.empty() && (token[token.size() - 1] == ' ' || token[token.size() - 1] == '\t')) {
                token.erase(token.end() - 1);
            }

            if (!token.empty()) {
                E one;
                if (!from_string_ci<E>(token.c_str(), one)) {
                    return false;
                }
                out |= one;
            }

            if (pos == std::string::npos) {
                break;
            }
            start = pos + 1;
        }

        return true;
    }

#define ENUM_UTIL_ENABLE_BITMASK(EnumType)                              \
    namespace enum_util {                                               \
        template <>                                                     \
        struct enable_bitmask_operators<EnumType> : std::true_type {};  \
    }                                                                   \
    using enum_util::operator|;                                         \
    using enum_util::operator&;                                         \
    using enum_util::operator^;                                         \
    using enum_util::operator~;                                         \
    using enum_util::operator|=;                                        \
    using enum_util::operator&=;                                        \
    using enum_util::operator^=;                                        \

#define ENUM_UTIL__ENTRY(EnumType, Name) {#Name, EnumType::Name},

#define ENUM_UTIL_REGISTER_ENUM(EnumType, ITEMS_MACRO, COUNT)   \
    namespace enum_util                                         \
    {                                                           \
        template <>                                             \
        struct enum_traits<EnumType>                            \
        {                                                       \
            static const enum_entry<EnumType> *entries()        \
            {                                                   \
                static const enum_entry<EnumType> e[] = {       \
                    ITEMS_MACRO(ENUM_UTIL__ENTRY)               \
                };                                              \
                return e;                                       \
            }                                                   \
            static std::size_t size()                           \
            {                                                   \
                return static_cast<std::size_t>(COUNT);         \
            }                                                   \
        };                                                      \
    }

} // namespace enum_util

#endif // __UTILS_ENUM_UTIL_HPP__
