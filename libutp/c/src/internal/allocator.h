#ifndef EULAR_UTP_INTERNAL_ALLOCATOR_H
#define EULAR_UTP_INTERNAL_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*utp_alloc_fn)(void *user_data, size_t size);
typedef void *(*utp_realloc_fn)(void *user_data, void *ptr, size_t size);
typedef void (*utp_free_fn)(void *user_data, void *ptr);

typedef struct utp_allocator {
  utp_alloc_fn alloc;
  utp_realloc_fn realloc;
  utp_free_fn free;
  void *user_data;
} utp_allocator_t;

const utp_allocator_t *utp_default_allocator(void);
const utp_allocator_t *utp_allocator_resolve(const utp_allocator_t *allocator);
void *utp_allocator_alloc(const utp_allocator_t *allocator, size_t size);
void *utp_allocator_realloc(const utp_allocator_t *allocator, void *ptr,
                            size_t size);
void utp_allocator_free(const utp_allocator_t *allocator, void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_INTERNAL_ALLOCATOR_H */
