#ifndef EULAR_UTP_INTERNAL_BUFFER_H
#define EULAR_UTP_INTERNAL_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "internal/allocator.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_buffer {
  uint8_t *data;
  size_t length;
  size_t capacity;
  size_t max_capacity;
  const utp_allocator_t *allocator;
} utp_buffer_t;

utp_internal_error_t utp_buffer_init(utp_buffer_t *buffer,
                                     const utp_allocator_t *allocator,
                                     size_t max_capacity);
void utp_buffer_cleanup(utp_buffer_t *buffer);
void utp_buffer_clear(utp_buffer_t *buffer);
utp_internal_error_t utp_buffer_reserve(utp_buffer_t *buffer, size_t capacity);
utp_internal_error_t utp_buffer_resize(utp_buffer_t *buffer, size_t length);
utp_internal_error_t utp_buffer_append(utp_buffer_t *buffer, const void *data,
                                       size_t length);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_INTERNAL_BUFFER_H */
