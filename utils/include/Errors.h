#ifndef __ERRORS_H__
#define __ERRORS_H__

#include <errno.h>

namespace eular {
typedef enum Status {
    OK                = 0,
    NO_ERROR          = OK,

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
} status_t;

} // namespace eular

#endif // __ERRORS_H__