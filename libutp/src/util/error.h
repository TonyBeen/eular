/*************************************************************************
    > File Name: error.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 10:30:36 AM CST
 ************************************************************************/

#ifndef __UTP_UTILS_ERROR_H__
#define __UTP_UTILS_ERROR_H__

#include <string>
#include <array>

#include "fmt/fmt.h"

#include "utp/errno.h"

static THREAD_LOCAL int32_t tls_last_error = 0;
static THREAD_LOCAL std::array<char, 512> tls_error_buf = {0};

void SetLastError(int32_t err_code, const char* fmt = nullptr, ...);

template <typename... Args>
inline void SetLastErrorV(int32_t err_code, fmt::format_string<Args...> format_str, Args&&... args) {
    tls_last_error = err_code;

    try {
        auto result =  fmt::format_to_n(tls_error_buf.data(), tls_error_buf.size() - 1, "{}: {}", format_str, std::forward<Args>(args)...);
        *result.out = '\0';
        // tls_error_buf[result.size] = '\0';
    } catch (const fmt::format_error& e) {
        constexpr const char* fallback = "[format error]";
        constexpr size_t fallback_len = 14;
        std::copy_n(fallback, fallback_len + 1, tls_error_buf.data());
    } catch (...) {
        tls_error_buf[0] = '\0';
    }
}

using OpenSSLErrorMsg = std::array<char, 256>;
OpenSSLErrorMsg GetOpenSSLErrorMsg(uint32_t &sslCode);

#endif // __UTP_UTILS_ERROR_H__
