#include "internal/error.h"

#include <errno.h>

bool utp_internal_error_is_ok(utp_internal_error_t error) {
  return error == UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_internal_error_from_errno(int system_error) {
  if (system_error <= 0 ||
      (uint32_t)system_error > UTP_INTERNAL_ERROR_VALUE_MASK) {
    return UTP_INTERNAL_ERROR_IO;
  }
  return UTP_INTERNAL_ERROR_FACILITY_POSIX | (uint32_t)system_error;
}

bool utp_internal_error_is_posix(utp_internal_error_t error) {
  return (error & ~UTP_INTERNAL_ERROR_VALUE_MASK) ==
         UTP_INTERNAL_ERROR_FACILITY_POSIX;
}

int utp_internal_error_to_errno(utp_internal_error_t error) {
  return utp_internal_error_is_posix(error)
             ? (int)(error & UTP_INTERNAL_ERROR_VALUE_MASK)
             : 0;
}

utp_status_t utp_internal_error_to_status(utp_internal_error_t error) {
  int system_error;

  switch (error) {
    case UTP_INTERNAL_ERROR_OK:
      return UTP_STATUS_OK;
    case UTP_INTERNAL_ERROR_INVALID_ARGUMENT:
      return UTP_STATUS_INVALID_ARGUMENT;
    case UTP_INTERNAL_ERROR_NOMEM:
      return UTP_STATUS_NOMEM;
    case UTP_INTERNAL_ERROR_LIMIT:
      return UTP_STATUS_LIMIT;
    case UTP_INTERNAL_ERROR_EXISTS:
      return UTP_STATUS_EXISTS;
    case UTP_INTERNAL_ERROR_NOT_FOUND:
      return UTP_STATUS_NOT_FOUND;
    case UTP_INTERNAL_ERROR_OVERFLOW:
      return UTP_STATUS_OVERFLOW;
    case UTP_INTERNAL_ERROR_STATE:
      return UTP_STATUS_STATE;
    case UTP_INTERNAL_ERROR_PROTOCOL:
      return UTP_STATUS_PROTOCOL;
    case UTP_INTERNAL_ERROR_CRYPTO:
      return UTP_STATUS_CRYPTO;
    case UTP_INTERNAL_ERROR_AUTH:
      return UTP_STATUS_AUTH;
    case UTP_INTERNAL_ERROR_UNSUPPORTED:
      return UTP_STATUS_UNSUPPORTED;
    case UTP_INTERNAL_ERROR_TIMEOUT:
      return UTP_STATUS_TIMEOUT;
    case UTP_INTERNAL_ERROR_CLOSED:
      return UTP_STATUS_CLOSED;
    case UTP_INTERNAL_ERROR_WOULD_BLOCK:
      return UTP_STATUS_WOULD_BLOCK;
    default:
      break;
  }
  system_error = utp_internal_error_to_errno(error);
  if (system_error == 0) {
    return UTP_STATUS_IO;
  }
#ifdef ENOMEM
  if (system_error == ENOMEM) {
    return UTP_STATUS_NOMEM;
  }
#endif
#ifdef EINVAL
  if (system_error == EINVAL) {
    return UTP_STATUS_INVALID_ARGUMENT;
  }
#endif
#ifdef EOVERFLOW
  if (system_error == EOVERFLOW) {
    return UTP_STATUS_OVERFLOW;
  }
#endif
#ifdef EAGAIN
  if (system_error == EAGAIN) {
    return UTP_STATUS_WOULD_BLOCK;
  }
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
  if (system_error == EWOULDBLOCK) {
    return UTP_STATUS_WOULD_BLOCK;
  }
#endif
#ifdef EINTR
  if (system_error == EINTR) {
    return UTP_STATUS_WOULD_BLOCK;
  }
#endif
#ifdef ETIMEDOUT
  if (system_error == ETIMEDOUT) {
    return UTP_STATUS_TIMEOUT;
  }
#endif
#ifdef ENOBUFS
  if (system_error == ENOBUFS) {
    return UTP_STATUS_LIMIT;
  }
#endif
#if defined(EACCES) && defined(EPERM)
  if (system_error == EACCES || system_error == EPERM) {
    return UTP_STATUS_AUTH;
  }
#elif defined(EACCES)
  if (system_error == EACCES) {
    return UTP_STATUS_AUTH;
  }
#elif defined(EPERM)
  if (system_error == EPERM) {
    return UTP_STATUS_AUTH;
  }
#endif
  return UTP_STATUS_IO;
}
