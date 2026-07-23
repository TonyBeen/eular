#include <utp/status.h>

const char *utp_status_string(utp_status_t status) {
  switch (status) {
    case UTP_STATUS_OK:
      return "ok";
    case UTP_STATUS_INVALID_ARGUMENT:
      return "invalid_argument";
    case UTP_STATUS_NOMEM:
      return "no_memory";
    case UTP_STATUS_LIMIT:
      return "limit";
    case UTP_STATUS_EXISTS:
      return "exists";
    case UTP_STATUS_NOT_FOUND:
      return "not_found";
    case UTP_STATUS_OVERFLOW:
      return "overflow";
    case UTP_STATUS_STATE:
      return "invalid_state";
    case UTP_STATUS_IO:
      return "io";
    case UTP_STATUS_WOULD_BLOCK:
      return "would_block";
    case UTP_STATUS_TIMEOUT:
      return "timeout";
    case UTP_STATUS_CLOSED:
      return "closed";
    case UTP_STATUS_PROTOCOL:
      return "protocol";
    case UTP_STATUS_CRYPTO:
      return "crypto";
    case UTP_STATUS_AUTH:
      return "authentication";
    case UTP_STATUS_UNSUPPORTED:
      return "unsupported";
    default:
      return "unknown";
  }
}
