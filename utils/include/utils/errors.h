#ifndef __ERRORS_H__
#define __ERRORS_H__

#include <stdint.h>
#include <errno.h>
#include <string>

#include <utils/sysdef.h>

#define STATUS(x) static_cast<status_t>(Status::x)

typedef int32_t status_t;
enum class Status : int32_t {
    OK                = 0,
    NO_ERROR          = OK,

    UNKNOWN_ERROR       = (-0xFFFF),

    NOT_INITIALIZED,    /* Uninitialized */
    NOT_IMPLEMENTED,    /* Function not implemented */

    NO_MEMORY           = -ENOMEM,      /* Out of memory */
    INVALID_OPERATION   = -ENOSYS,      /* Illegal operation */
    INVALID_PARAM       = -EINVAL,      /* Invalid argument */
    NOT_FOUND           = -ENOENT,      /* No such file or directory */
    OPT_NOT_PERMITTED   = -EPERM,       /* Operation not permitted */
    PERMISSION_DENIED   = -EACCES,      /* Permission denied */
    ALREADY_EXISTS      = -EEXIST,      /* File exists */
    DEAD_OBJECT         = -EPIPE,       /* Object invalid */
    BAD_INDEX           = -EOVERFLOW,   /* Value too large for defined data type */
    NOT_ENOUGH_DATA     = -ENODATA,     /* No data available */
    WOULD_BLOCK         = -EWOULDBLOCK, /* Operation would block */
    TIMED_OUT           = -ETIMEDOUT,   /* Something timed out */
    NOT_SUPPORT         = -EOPNOTSUPP,  /* Operation not supported on transport endpoint */
    NO_SPACE            = -ENOBUFS,     /* No buffer space available */
    MESSAGE_TOO_LONG    = -EMSGSIZE,    /* Message too long */
    UNEXPECTED_NULL     = -EFAULT,      /* Unexpected null pointer */
};

EXTERN_C_BEGIN

UTILS_API int32_t       GetLastErrno();
UTILS_API const char*   StatusToString(status_t status);

EXTERN_C_END

UTILS_API std::string   FormatErrno(int32_t status);

#endif // __ERRORS_H__