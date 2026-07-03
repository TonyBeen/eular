#include "error.h"

#include "commom.h"

#include <string.h>
#include <errno.h>
#include <stdarg.h>

#include <array>
#include <type_traits>

#include <openssl/err.h>

void SetLastError(int32_t err_code, const char *fmt, ...)
{
    tls_last_error = err_code;

    va_list args;
    va_start(args, fmt);
    vsnprintf(tls_error_buf.data(), tls_error_buf.size() - 1, fmt, args);
    va_end(args);
}

OpenSSLErrorMsg GetOpenSSLErrorMsg(uint32_t &sslCode)
{
    sslCode = ERR_get_error();
    OpenSSLErrorMsg buf;
    buf[0] = '\0';
    if (sslCode == 0) {
        return buf;
    }

    ERR_error_string_n(sslCode, buf.data(), buf.size());
    return buf;
}

int32_t GetSystemLastError()
{
#if defined(OS_WINDOWS)
    return static_cast<int32_t>(WSAGetLastError());
#else
    return errno;
#endif
}

namespace {
// Helper functions to handle both GNU and XSI versions of strerror_r
inline char* strerror_r_helper(char* res, char* /*buf*/) { return res; }
inline char* strerror_r_helper(int /*res*/, char* buf) { return buf; }
}

ErrnoMsg GetSystemErrnoMsg(int32_t status)
{
    ErrnoMsg buf;
    std::array<char, 512> buffer;
    buffer[0] = '\0';
#if defined(OS_WINDOWS)
    strerror_s(buffer.data(), buffer.size(), status);
#else
    char *errorMsg = strerror_r_helper(strerror_r(status, buffer.data(), buffer.size()), buffer.data());
    if (errorMsg && errorMsg != buffer.data()) {
        buffer[0] = '\0';
        strncat(buffer.data(), errorMsg, buffer.size() - 1);
    }
#endif
    buf = buffer.data();
    return buf;
}
