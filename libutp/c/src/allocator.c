#include "internal/allocator.h"

#include <stdlib.h>

static void *default_alloc(void *user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

static void *default_realloc(void *user_data, void *ptr, size_t size) {
  (void)user_data;
  return realloc(ptr, size);
}

static void default_free(void *user_data, void *ptr) {
  (void)user_data;
  free(ptr);
}

static const utp_allocator_t k_default_allocator = {
    default_alloc,
    default_realloc,
    default_free,
    NULL,
};

const utp_allocator_t *utp_default_allocator(void) {
  return &k_default_allocator;
}

const utp_allocator_t *utp_allocator_resolve(const utp_allocator_t *allocator) {
  if (allocator == NULL) {
    return utp_default_allocator();
  }
  if (allocator->alloc == NULL || allocator->realloc == NULL ||
      allocator->free == NULL) {
    return NULL;
  }
  return allocator;
}

void *utp_allocator_alloc(const utp_allocator_t *allocator, size_t size) {
  const utp_allocator_t *resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL || size == 0) {
    return NULL;
  }
  return resolved->alloc(resolved->user_data, size);
}

void *utp_allocator_realloc(const utp_allocator_t *allocator, void *ptr,
                            size_t size) {
  const utp_allocator_t *resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL || size == 0) {
    return NULL;
  }
  return resolved->realloc(resolved->user_data, ptr, size);
}

void utp_allocator_free(const utp_allocator_t *allocator, void *ptr) {
  const utp_allocator_t *resolved = utp_allocator_resolve(allocator);
  if (resolved != NULL && ptr != NULL) {
    resolved->free(resolved->user_data, ptr);
  }
}
