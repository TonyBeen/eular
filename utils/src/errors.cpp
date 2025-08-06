/*************************************************************************
    > File Name: errors.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月31日 星期四 20时46分22秒
 ************************************************************************/

#include "utils/errors.h"

#if defined(OS_LINUX) || defined(OS_MACOS)
#include <string.h>
#elif defined(OS_WINDOWS)
#include <windows.h>
#endif

int32_t GetLastErrno()
{
    return errno;
}

#define XXX(code, msg) \
    case Status::code: \
        return msg;

const char *StatusToString(status_t status)
{
    switch ((Status)status) {
        XXX(OK, "OK")

        XXX(NOT_INITIALIZED, "Uninitialized")
        XXX(NOT_IMPLEMENTED, "Function not implemented")

        XXX(NO_MEMORY, "Out of memory")
        XXX(INVALID_OPERATION, "Illegal operation")
        XXX(INVALID_PARAM, "Invalid argument")
        XXX(NOT_FOUND, "No such file or directory")
        XXX(OPT_NOT_PERMITTED, "Operation not permitted")
        XXX(PERMISSION_DENIED, "Permission denied")
        XXX(ALREADY_EXISTS, "File exists")
        XXX(DEAD_OBJECT, "Object invalid")
        XXX(BAD_INDEX, "Value too large for defined data type")
        XXX(NOT_ENOUGH_DATA, "No data available")
        XXX(WOULD_BLOCK, "Operation would block")
        XXX(TIMED_OUT, "Something timed out")
        XXX(NOT_SUPPORT, "Operation not supported on transport endpoint")
        XXX(NO_SPACE, "No buffer space available")
        XXX(MESSAGE_TOO_LONG, "Message too long")
        XXX(UNEXPECTED_NULL, "Unexpected null pointer")
        default:
            return "Unknown error";
    }
#undef XXX
}

std::string FormatErrno(int32_t status)
{
#if defined(OS_LINUX) || defined(OS_MACOS)
    return std::string(strerror(status));
#elif defined(OS_WINDOWS)
    LPSTR lpMsgBuf;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, status, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&lpMsgBuf, 0, NULL);
    std::string msg(lpMsgBuf);
    LocalFree(lpMsgBuf);
    return msg;
#else
    return "Unsupported platform";
#endif
}
