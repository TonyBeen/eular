#ifndef EULAR_UTP_INTERNAL_RANGE_SET_H
#define EULAR_UTP_INTERNAL_RANGE_SET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/allocator.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_range {
  uint64_t start;
  uint64_t end;
} utp_range_t;

typedef struct utp_range_set {
  utp_range_t *ranges;
  size_t count;
  size_t capacity;
  size_t max_ranges;
  const utp_allocator_t *allocator;
} utp_range_set_t;

utp_internal_error_t utp_range_set_init(utp_range_set_t *set,
                                        const utp_allocator_t *allocator,
                                        size_t max_ranges);
void utp_range_set_cleanup(utp_range_set_t *set);
void utp_range_set_clear(utp_range_set_t *set);
utp_internal_error_t utp_range_set_insert(utp_range_set_t *set, uint64_t start,
                                          uint64_t end);
bool utp_range_set_contains(const utp_range_set_t *set, uint64_t value);
const utp_range_t *utp_range_set_at(const utp_range_set_t *set, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_INTERNAL_RANGE_SET_H */
