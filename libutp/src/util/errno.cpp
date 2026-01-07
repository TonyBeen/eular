/*************************************************************************
    > File Name: errno.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 10:23:33 AM CST
 ************************************************************************/

#include "util/error.h"

#include <string>

UTP_API int32_t GetLastError()
{
    return tls_last_error;
}

UTP_API const char *GetErrorString()
{
    return tls_error_buf.data();
}