#include "internal/buffer.h"

#include <limits.h>
#include <string.h>

static size_t next_capacity(size_t current, size_t required, size_t maximum) {
  size_t capacity = current == 0 ? (maximum < 64u ? maximum : 64u) : current;

  while (capacity < required) {
    if (capacity > maximum / 2u) {
      return maximum;
    }
    capacity *= 2u;
  }
  return capacity;
}

utp_internal_error_t utp_buffer_init(utp_buffer_t *buffer,
                                     const utp_allocator_t *allocator,
                                     size_t max_capacity) {
  const utp_allocator_t *resolved;

  if (buffer == NULL || max_capacity == 0) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  memset(buffer, 0, sizeof(*buffer));
  buffer->max_capacity = max_capacity;
  buffer->allocator = resolved;
  return UTP_INTERNAL_ERROR_OK;
}

void utp_buffer_cleanup(utp_buffer_t *buffer) {
  if (buffer == NULL) {
    return;
  }
  utp_allocator_free(buffer->allocator, buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

void utp_buffer_clear(utp_buffer_t *buffer) {
  if (buffer != NULL) {
    buffer->length = 0;
  }
}

utp_internal_error_t utp_buffer_reserve(utp_buffer_t *buffer, size_t capacity) {
  uint8_t *new_data;
  size_t next;

  if (buffer == NULL || buffer->allocator == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (capacity > buffer->max_capacity) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  if (capacity <= buffer->capacity) {
    return UTP_INTERNAL_ERROR_OK;
  }
  next = next_capacity(buffer->capacity, capacity, buffer->max_capacity);
  if (next < capacity || next > buffer->max_capacity ||
      next > SIZE_MAX / sizeof(*new_data)) {
    return UTP_INTERNAL_ERROR_OVERFLOW;
  }
  new_data = utp_allocator_realloc(buffer->allocator, buffer->data, next);
  if (new_data == NULL) {
    return UTP_INTERNAL_ERROR_NOMEM;
  }
  buffer->data = new_data;
  buffer->capacity = next;
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_buffer_resize(utp_buffer_t *buffer, size_t length) {
  utp_internal_error_t status;

  if (buffer == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  status = utp_buffer_reserve(buffer, length);
  if (status != UTP_INTERNAL_ERROR_OK) {
    return status;
  }
  if (length > buffer->length) {
    memset(buffer->data + buffer->length, 0, length - buffer->length);
  }
  buffer->length = length;
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_buffer_append(utp_buffer_t *buffer, const void *data,
                                       size_t length) {
  utp_internal_error_t status;
  size_t required;

  if (buffer == NULL || (data == NULL && length != 0)) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (length == 0) {
    return UTP_INTERNAL_ERROR_OK;
  }
  if (length > SIZE_MAX - buffer->length) {
    return UTP_INTERNAL_ERROR_OVERFLOW;
  }
  required = buffer->length + length;
  status = utp_buffer_reserve(buffer, required);
  if (status != UTP_INTERNAL_ERROR_OK) {
    return status;
  }
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length = required;
  return UTP_INTERNAL_ERROR_OK;
}
