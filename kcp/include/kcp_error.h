#ifndef __KCP_ERROR_H__
#define __KCP_ERROR_H__

#include <stdint.h>
#include <errno.h>

enum KcpError {
    NO_ERROR = 0,

    // General errors
    UNKNOWN_ERROR       = (-0x7FFFFFFF), // INT32_MIN value

    NO_MEMORY           = -ENOMEM,      /* Out of memory */
    INVALID_OPERATION   = -ENOSYS,      /* Illegal operation */
    INVALID_PARAM       = -EINVAL,      /* Invalid argument */
    NOT_FOUND           = -ENOENT,      /* No such file or directory */
    OPT_NOT_PERMITTED   = -EPERM,       /* Operation not permitted */
    PERMISSION_DENIED   = -EACCES,      /* Permission denied */
    NO_INIT             = -ENODEV,      /* Uninitialized */
    ALREADY_EXISTS      = -EEXIST,      /* File exists */
    DEAD_OBJECT         = -EPIPE,       /* Object invalid */
    BAD_INDEX           = -EOVERFLOW,   /* Value too large for defined data type */
    NOT_ENOUGH_DATA     = -ENODATA,     /* No data available */
    WOULD_BLOCK         = -EWOULDBLOCK, /* Operation would block */
    TIMED_OUT           = -ETIMEDOUT,   /* Something timed out */
    NOT_SUPPORT         = -EOPNOTSUPP,  /* Operation not supported on transport endpoint */
    NO_MORE_ITEM        = -ENOBUFS,     /* No buffer space available */

    // I/O errors
    READ_ERROR          = -11000,
    WRITE_ERROR,
    CLOSE_ERROR,
    ADD_EVENT_ERROR,

    // Network errors
    CREATE_SOCKET_ERROR = -110000,
    BIND_ERROR,
    SOCKET_INUSE,
    LISTEN_ERROR,
    ACCEPT_ERROR,
    NO_PENDING_CONNECTION,
    CONNECT_ERROR,
    UNKNOWN_PROTO,
    PROTO_ERROR,
    IOCTL_ERROR,
};
typedef int32_t kcp_error_t;

#endif // __KCP_ERROR_H__
