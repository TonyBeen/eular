/*************************************************************************
    > File Name: errno.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 10:23:33 AM CST
 ************************************************************************/

#include "util/error.h"

#include <string>

UTP_THREAD_LOCAL int32_t tls_last_error = 0;
UTP_THREAD_LOCAL std::array<char, 512> tls_error_buf = {0};

UTP_API int32_t GetLastError()
{
    return tls_last_error;
}

UTP_API const char *GetErrorString()
{
    return tls_error_buf.data();
}