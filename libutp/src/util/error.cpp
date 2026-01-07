#include "error.h"

#include <stdarg.h>

#include <array>
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
