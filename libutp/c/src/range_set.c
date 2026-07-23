#include "internal/range_set.h"

#include <limits.h>
#include <string.h>

static utp_internal_error_t reserve_ranges(utp_range_set_t *set,
                                           size_t required) {
  utp_range_t *ranges;
  size_t capacity;

  if (required > set->max_ranges) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  if (required <= set->capacity) {
    return UTP_INTERNAL_ERROR_OK;
  }
  capacity = set->capacity == 0 ? (set->max_ranges < 8u ? set->max_ranges : 8u)
                                : set->capacity;
  while (capacity < required) {
    if (capacity > set->max_ranges / 2u) {
      capacity = set->max_ranges;
      break;
    }
    capacity *= 2u;
  }
  if (capacity < required || capacity > SIZE_MAX / sizeof(*ranges)) {
    return UTP_INTERNAL_ERROR_OVERFLOW;
  }
  ranges = utp_allocator_realloc(set->allocator, set->ranges,
                                 capacity * sizeof(*ranges));
  if (ranges == NULL) {
    return UTP_INTERNAL_ERROR_NOMEM;
  }
  set->ranges = ranges;
  set->capacity = capacity;
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_range_set_init(utp_range_set_t *set,
                                        const utp_allocator_t *allocator,
                                        size_t max_ranges) {
  const utp_allocator_t *resolved;

  if (set == NULL || max_ranges == 0) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  memset(set, 0, sizeof(*set));
  set->max_ranges = max_ranges;
  set->allocator = resolved;
  return UTP_INTERNAL_ERROR_OK;
}

void utp_range_set_cleanup(utp_range_set_t *set) {
  if (set == NULL) {
    return;
  }
  utp_allocator_free(set->allocator, set->ranges);
  memset(set, 0, sizeof(*set));
}

void utp_range_set_clear(utp_range_set_t *set) {
  if (set != NULL) {
    set->count = 0;
  }
}

utp_internal_error_t utp_range_set_insert(utp_range_set_t *set, uint64_t start,
                                          uint64_t end) {
  size_t first;
  size_t after;
  size_t removed;
  utp_internal_error_t status;

  if (set == NULL || start >= end) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  first = 0;
  while (first < set->count && set->ranges[first].end < start) {
    ++first;
  }
  after = first;
  while (after < set->count && set->ranges[after].start <= end) {
    if (set->ranges[after].start < start) {
      start = set->ranges[after].start;
    }
    if (set->ranges[after].end > end) {
      end = set->ranges[after].end;
    }
    ++after;
  }
  removed = after - first;
  if (removed == 0) {
    if (set->count >= set->max_ranges) {
      return UTP_INTERNAL_ERROR_LIMIT;
    }
    status = reserve_ranges(set, set->count + 1u);
    if (status != UTP_INTERNAL_ERROR_OK) {
      return status;
    }
    if (first < set->count) {
      memmove(&set->ranges[first + 1u], &set->ranges[first],
              (set->count - first) * sizeof(*set->ranges));
    }
    ++set->count;
  } else if (removed > 1u) {
    memmove(&set->ranges[first + 1u], &set->ranges[after],
            (set->count - after) * sizeof(*set->ranges));
    set->count -= removed - 1u;
  }
  set->ranges[first].start = start;
  set->ranges[first].end = end;
  return UTP_INTERNAL_ERROR_OK;
}

bool utp_range_set_contains(const utp_range_set_t *set, uint64_t value) {
  size_t left = 0;
  size_t right;

  if (set == NULL) {
    return false;
  }
  right = set->count;
  while (left < right) {
    size_t middle = left + (right - left) / 2u;
    const utp_range_t *range = &set->ranges[middle];
    if (value < range->start) {
      right = middle;
    } else if (value >= range->end) {
      left = middle + 1u;
    } else {
      return true;
    }
  }
  return false;
}

const utp_range_t *utp_range_set_at(const utp_range_set_t *set, size_t index) {
  if (set == NULL || index >= set->count) {
    return NULL;
  }
  return &set->ranges[index];
}
