#ifndef EULAR_UTP_INTERNAL_RING_H
#define EULAR_UTP_INTERNAL_RING_H

#include <stddef.h>

#include "internal/allocator.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_ring {
  unsigned char *data;
  size_t element_size;
  size_t capacity;
  size_t head;
  size_t count;
  const utp_allocator_t *allocator;
} utp_ring_t;

utp_internal_error_t utp_ring_init(utp_ring_t *ring,
                                   const utp_allocator_t *allocator,
                                   size_t element_size, size_t capacity);
void utp_ring_cleanup(utp_ring_t *ring);
void utp_ring_clear(utp_ring_t *ring);
size_t utp_ring_count(const utp_ring_t *ring);
size_t utp_ring_capacity(const utp_ring_t *ring);
utp_internal_error_t utp_ring_push(utp_ring_t *ring, const void *element);
utp_internal_error_t utp_ring_peek(const utp_ring_t *ring, void *element);
utp_internal_error_t utp_ring_pop(utp_ring_t *ring, void *element);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_INTERNAL_RING_H */
