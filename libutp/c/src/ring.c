#include "internal/ring.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

utp_internal_error_t utp_ring_init(utp_ring_t *ring,
                                   const utp_allocator_t *allocator,
                                   size_t element_size, size_t capacity) {
  const utp_allocator_t *resolved;

  if (ring == NULL || element_size == 0 || capacity == 0 ||
      capacity > SIZE_MAX / element_size) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  memset(ring, 0, sizeof(*ring));
  ring->data = utp_allocator_alloc(resolved, element_size * capacity);
  if (ring->data == NULL) {
    return UTP_INTERNAL_ERROR_NOMEM;
  }
  ring->element_size = element_size;
  ring->capacity = capacity;
  ring->allocator = resolved;
  return UTP_INTERNAL_ERROR_OK;
}

void utp_ring_cleanup(utp_ring_t *ring) {
  if (ring == NULL) {
    return;
  }
  utp_allocator_free(ring->allocator, ring->data);
  memset(ring, 0, sizeof(*ring));
}

void utp_ring_clear(utp_ring_t *ring) {
  if (ring != NULL) {
    ring->head = 0;
    ring->count = 0;
  }
}

size_t utp_ring_count(const utp_ring_t *ring) {
  return ring == NULL ? 0 : ring->count;
}

size_t utp_ring_capacity(const utp_ring_t *ring) {
  return ring == NULL ? 0 : ring->capacity;
}

utp_internal_error_t utp_ring_push(utp_ring_t *ring, const void *element) {
  size_t index;

  if (ring == NULL || element == NULL || ring->data == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (ring->count == ring->capacity) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  if (ring->head >= ring->capacity - ring->count) {
    index = ring->head - (ring->capacity - ring->count);
  } else {
    index = ring->head + ring->count;
  }
  memcpy(ring->data + index * ring->element_size, element, ring->element_size);
  ++ring->count;
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_ring_peek(const utp_ring_t *ring, void *element) {
  if (ring == NULL || element == NULL || ring->data == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (ring->count == 0) {
    return UTP_INTERNAL_ERROR_NOT_FOUND;
  }
  memcpy(element, ring->data + ring->head * ring->element_size,
         ring->element_size);
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_ring_pop(utp_ring_t *ring, void *element) {
  utp_internal_error_t status;

  if (ring == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  status = utp_ring_peek(ring, element);
  if (status != UTP_INTERNAL_ERROR_OK) {
    return status;
  }
  ring->head = (ring->head + 1u) % ring->capacity;
  --ring->count;
  return UTP_INTERNAL_ERROR_OK;
}
