#ifndef EULAR_UTP_STATUS_H
#define EULAR_UTP_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum utp_status {
  UTP_STATUS_OK = 0,
  UTP_STATUS_INVALID_ARGUMENT = -1,
  UTP_STATUS_NOMEM = -2,
  UTP_STATUS_LIMIT = -3,
  UTP_STATUS_EXISTS = -4,
  UTP_STATUS_NOT_FOUND = -5,
  UTP_STATUS_OVERFLOW = -6,
  UTP_STATUS_STATE = -7,
  UTP_STATUS_IO = -8,
  UTP_STATUS_WOULD_BLOCK = -9,
  UTP_STATUS_TIMEOUT = -10,
  UTP_STATUS_CLOSED = -11,
  UTP_STATUS_PROTOCOL = -12,
  UTP_STATUS_CRYPTO = -13,
  UTP_STATUS_AUTH = -14,
  UTP_STATUS_UNSUPPORTED = -15
} utp_status_t;

const char *utp_status_string(utp_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_STATUS_H */
