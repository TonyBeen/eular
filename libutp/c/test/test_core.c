#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utp/utp.h>

#include "internal/allocator.h"
#include "internal/buffer.h"
#include "internal/error.h"
#include "internal/hash.h"
#include "internal/log.h"
#include "internal/range_set.h"
#include "internal/ring.h"

typedef struct fail_allocator {
  size_t calls;
  size_t fail_after;
} fail_allocator_t;

typedef struct hash_item {
  utp_hash_node_t node;
  uint32_t key;
  int present;
} hash_item_t;

typedef struct removal_state {
  size_t count;
} removal_state_t;

typedef struct log_capture {
  size_t calls;
  utp_status_t status;
  int system_error;
  char tag[UTP_LOG_TAG_MAX_LENGTH + 1u];
} log_capture_t;

static void capture_log(const utp_log_event_t *event, void *user_data) {
  log_capture_t *capture = user_data;

  ++capture->calls;
  capture->status = event->status;
  capture->system_error = event->system_error;
  assert(event->tag_length <= UTP_LOG_TAG_MAX_LENGTH);
  memcpy(capture->tag, event->tag, event->tag_length);
  capture->tag[event->tag_length] = '\0';
}

static void *fail_alloc(void *user_data, size_t size) {
  fail_allocator_t *state = user_data;
  ++state->calls;
  if (state->calls > state->fail_after) {
    return NULL;
  }
  return malloc(size);
}

static void *fail_realloc(void *user_data, void *ptr, size_t size) {
  fail_allocator_t *state = user_data;
  ++state->calls;
  if (state->calls > state->fail_after) {
    return NULL;
  }
  return realloc(ptr, size);
}

static void fail_free(void *user_data, void *ptr) {
  (void)user_data;
  free(ptr);
}

static utp_allocator_t make_fail_allocator(fail_allocator_t *state) {
  const utp_allocator_t allocator = {
      fail_alloc,
      fail_realloc,
      fail_free,
      state,
  };
  return allocator;
}

static uint64_t hash_key(uint32_t key) {
  uint64_t value = key;
  value ^= value >> 16u;
  value *= UINT64_C(0x7feb352d);
  value ^= value >> 15u;
  value *= UINT64_C(0x846ca68b);
  value ^= value >> 16u;
  return value;
}

static hash_item_t *item_from_node(const utp_hash_node_t *node) {
  return (hash_item_t *)((char *)node - offsetof(hash_item_t, node));
}

static bool hash_matches(const utp_hash_node_t *node, const void *key,
                         void *user_data) {
  const uint32_t *expected = key;
  const hash_item_t *item = item_from_node(node);
  (void)user_data;
  return item->key == *expected;
}

static void count_removed_node(utp_hash_node_t *node, void *user_data) {
  removal_state_t *state = user_data;

  assert(node->next == NULL);
  assert(node->prev_next == NULL);
  assert(node->table == NULL);
  ++state->count;
}

static uint32_t random_next(uint32_t *state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

static void test_version_and_status(void) {
  assert(strcmp(utp_version(), "1.0.0") == 0);
  assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
  assert(strcmp(utp_status_string(UTP_STATUS_NOMEM), "no_memory") == 0);
  assert(strcmp(utp_status_string(UTP_STATUS_PROTOCOL), "protocol") == 0);
  assert(strcmp(utp_status_string((utp_status_t)100), "unknown") == 0);
}

static void test_internal_error_and_log_tag(void) {
  utp_log_tag_t context_tag;
  utp_log_tag_t connection_tag;
  utp_log_tag_t send_control_tag;
  utp_logger_t logger;
  log_capture_t capture = {0};
  utp_internal_error_t error = utp_internal_error_from_errno(EAGAIN);
  char oversized_tag[UTP_LOG_TAG_MAX_LENGTH + 1u] = {0};

  assert(utp_internal_error_is_posix(error));
  assert(utp_internal_error_to_errno(error) == EAGAIN);
  assert(utp_internal_error_to_status(error) == UTP_STATUS_WOULD_BLOCK);
  assert(utp_internal_error_to_status(UTP_INTERNAL_ERROR_PROTOCOL) ==
         UTP_STATUS_PROTOCOL);
  assert(utp_log_tag_init(&context_tag, "context 1", 9u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(utp_log_tag_append(&connection_tag, &context_tag,
                            "connection scid 21313",
                            21u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_log_tag_append(&send_control_tag, &connection_tag, "send_control",
                            12u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_log_tag_init(&context_tag, oversized_tag, sizeof(oversized_tag)) ==
         UTP_INTERNAL_ERROR_LIMIT);
  logger.sink = capture_log;
  logger.user_data = &capture;
  utp_internal_log_error(&logger, &send_control_tag, error, "udp send failed");
  assert(capture.calls == 1u);
  assert(strcmp(capture.tag,
                "[context 1][connection scid 21313][send_control]") == 0);
  assert(capture.status == UTP_STATUS_WOULD_BLOCK &&
         capture.system_error == EAGAIN);
}

static void test_buffer(void) {
  utp_buffer_t buffer;
  const char first[] = "abc";
  const char second[] = "def";

  assert(utp_buffer_init(&buffer, NULL, 16) == UTP_INTERNAL_ERROR_OK);
  assert(utp_buffer_append(&buffer, first, sizeof(first) - 1u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(utp_buffer_append(&buffer, second, sizeof(second) - 1u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(buffer.length == 6u);
  assert(memcmp(buffer.data, "abcdef", 6u) == 0);
  assert(utp_buffer_resize(&buffer, 9u) == UTP_INTERNAL_ERROR_OK);
  assert(buffer.data[6] == 0 && buffer.data[7] == 0 && buffer.data[8] == 0);
  assert(utp_buffer_append(&buffer, "0123456789", 10u) ==
         UTP_INTERNAL_ERROR_LIMIT);
  assert(buffer.length == 9u);
  utp_buffer_clear(&buffer);
  assert(buffer.length == 0u);
  utp_buffer_cleanup(&buffer);
  utp_buffer_cleanup(&buffer);
}

static void test_buffer_allocation_failure(void) {
  fail_allocator_t state = {0u, 0u};
  utp_allocator_t allocator = make_fail_allocator(&state);
  utp_buffer_t buffer;

  assert(utp_buffer_init(&buffer, &allocator, 64u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_buffer_append(&buffer, "x", 1u) == UTP_INTERNAL_ERROR_NOMEM);
  assert(buffer.data == NULL && buffer.length == 0u && buffer.capacity == 0u);
  state.calls = 0u;
  state.fail_after = 1u;
  assert(utp_buffer_append(&buffer, "x", 1u) == UTP_INTERNAL_ERROR_OK);
  assert(buffer.length == 1u && buffer.data[0] == (uint8_t)'x');
  utp_buffer_cleanup(&buffer);
}

static void test_ring(void) {
  utp_ring_t ring;
  int value;
  int output;

  assert(utp_ring_init(&ring, NULL, sizeof(value), 3u) ==
         UTP_INTERNAL_ERROR_OK);
  value = 1;
  assert(utp_ring_push(&ring, &value) == UTP_INTERNAL_ERROR_OK);
  value = 2;
  assert(utp_ring_push(&ring, &value) == UTP_INTERNAL_ERROR_OK);
  value = 3;
  assert(utp_ring_push(&ring, &value) == UTP_INTERNAL_ERROR_OK);
  assert(utp_ring_push(&ring, &value) == UTP_INTERNAL_ERROR_LIMIT);
  assert(utp_ring_peek(&ring, &output) == UTP_INTERNAL_ERROR_OK && output == 1);
  assert(utp_ring_pop(&ring, &output) == UTP_INTERNAL_ERROR_OK && output == 1);
  value = 4;
  assert(utp_ring_push(&ring, &value) == UTP_INTERNAL_ERROR_OK);
  assert(utp_ring_pop(&ring, &output) == UTP_INTERNAL_ERROR_OK && output == 2);
  assert(utp_ring_pop(&ring, &output) == UTP_INTERNAL_ERROR_OK && output == 3);
  assert(utp_ring_pop(&ring, &output) == UTP_INTERNAL_ERROR_OK && output == 4);
  assert(utp_ring_pop(&ring, &output) == UTP_INTERNAL_ERROR_NOT_FOUND);
  utp_ring_cleanup(&ring);
  utp_ring_cleanup(&ring);
}

static void test_ring_allocation_failure(void) {
  fail_allocator_t state = {0u, 0u};
  utp_allocator_t allocator = make_fail_allocator(&state);
  utp_ring_t ring;
  int value = 1;

  assert(utp_ring_init(&ring, &allocator, sizeof(value), 4u) ==
         UTP_INTERNAL_ERROR_NOMEM);
  assert(ring.data == NULL && ring.capacity == 0u && ring.count == 0u &&
         ring.allocator == NULL);
  utp_ring_cleanup(&ring);
}

static void test_range_set(void) {
  utp_range_set_t set;
  utp_range_set_t single_range_set;
  const utp_range_t *range;

  assert(utp_range_set_init(&set, NULL, 4u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 10u, 20u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 30u, 40u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 20u, 30u) == UTP_INTERNAL_ERROR_OK);
  assert(set.count == 1u);
  range = utp_range_set_at(&set, 0u);
  assert(range != NULL && range->start == 10u && range->end == 40u);
  assert(utp_range_set_contains(&set, 10u));
  assert(utp_range_set_contains(&set, 39u));
  assert(!utp_range_set_contains(&set, 40u));
  assert(utp_range_set_insert(&set, 1u, 2u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 4u, 5u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 7u, 8u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 50u, 51u) == UTP_INTERNAL_ERROR_LIMIT);
  assert(utp_range_set_insert(&set, 9u, 9u) ==
         UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
  utp_range_set_cleanup(&set);
  utp_range_set_cleanup(&set);

  assert(utp_range_set_init(&single_range_set, NULL, 1u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&single_range_set, 1u, 2u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(single_range_set.capacity == 1u);
  assert(utp_range_set_insert(&single_range_set, 3u, 4u) ==
         UTP_INTERNAL_ERROR_LIMIT);
  utp_range_set_cleanup(&single_range_set);
}

static void test_range_set_random(void) {
  enum { RANGE_LIMIT = 128, VALUE_LIMIT = 256, OPERATIONS = 100000 };
  utp_range_set_t set;
  unsigned char expected[VALUE_LIMIT] = {0};
  uint32_t state = UINT32_C(0x21ab47d3);
  size_t operation;

  assert(utp_range_set_init(&set, NULL, RANGE_LIMIT) == UTP_INTERNAL_ERROR_OK);
  for (operation = 0; operation < OPERATIONS; ++operation) {
    uint32_t first = random_next(&state) % VALUE_LIMIT;
    uint32_t second = random_next(&state) % VALUE_LIMIT;
    uint32_t value;
    if (first > second) {
      uint32_t temporary = first;
      first = second;
      second = temporary;
    }
    if (first == second) {
      second = (second + 1u) % VALUE_LIMIT;
      if (second <= first) {
        first = 0u;
        second = VALUE_LIMIT;
      }
    }
    assert(utp_range_set_insert(&set, first, second) == UTP_INTERNAL_ERROR_OK);
    for (value = first; value < second; ++value) {
      expected[value] = 1u;
    }
    for (value = 0; value < VALUE_LIMIT; ++value) {
      assert(utp_range_set_contains(&set, value) == (expected[value] != 0u));
    }
  }
  utp_range_set_cleanup(&set);
}

static void test_range_set_allocation_failure(void) {
  fail_allocator_t state = {0u, 0u};
  utp_allocator_t allocator = make_fail_allocator(&state);
  utp_range_set_t set;
  utp_range_t *saved_ranges;
  size_t index;
  size_t saved_capacity;

  assert(utp_range_set_init(&set, &allocator, 16u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_range_set_insert(&set, 0u, 1u) == UTP_INTERNAL_ERROR_NOMEM);
  assert(set.ranges == NULL && set.count == 0u && set.capacity == 0u);

  state.calls = 0u;
  state.fail_after = 1u;
  for (index = 0; index < 8u; ++index) {
    uint64_t start = (uint64_t)index * 2u;
    assert(utp_range_set_insert(&set, start, start + 1u) ==
           UTP_INTERNAL_ERROR_OK);
  }
  assert(set.count == 8u && set.capacity == 8u);
  saved_ranges = set.ranges;
  saved_capacity = set.capacity;
  state.fail_after = state.calls;
  assert(utp_range_set_insert(&set, 16u, 17u) == UTP_INTERNAL_ERROR_NOMEM);
  assert(set.ranges == saved_ranges && set.count == 8u &&
         set.capacity == saved_capacity);
  for (index = 0; index < 8u; ++index) {
    assert(utp_range_set_contains(&set, (uint64_t)index * 2u));
  }
  state.fail_after = SIZE_MAX;
  assert(utp_range_set_insert(&set, 16u, 17u) == UTP_INTERNAL_ERROR_OK);
  assert(set.count == 9u && set.capacity == 16u);
  utp_range_set_cleanup(&set);
}

static void test_hash(void) {
  enum { ITEM_COUNT = 128 };
  hash_item_t items[ITEM_COUNT];
  hash_item_t duplicate;
  utp_hash_table_t table;
  size_t index;
  size_t seen = 0;
  utp_hash_iter_t iter;
  utp_hash_node_t *node;

  memset(items, 0, sizeof(items));
  memset(&duplicate, 0, sizeof(duplicate));
  assert(utp_hash_table_init(&table, NULL, ITEM_COUNT) ==
         UTP_INTERNAL_ERROR_OK);
  for (index = 0; index < ITEM_COUNT; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
    assert(utp_hash_table_insert(&table, &items[index].node,
                                 hash_key(items[index].key), &items[index].key,
                                 hash_matches, NULL) == UTP_INTERNAL_ERROR_OK);
    items[index].present = 1;
  }
  assert(utp_hash_table_count(&table) == ITEM_COUNT);
  assert(utp_hash_table_insert(&table, &items[0].node, hash_key(items[0].key),
                               &items[0].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_STATE);
  duplicate.key = items[0].key;
  utp_hash_node_init(&duplicate.node);
  assert(utp_hash_table_insert(&table, &duplicate.node, hash_key(duplicate.key),
                               &duplicate.key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_EXISTS);
  assert(duplicate.node.prev_next == NULL && duplicate.node.table == NULL);
  for (index = 0; index < ITEM_COUNT; ++index) {
    node = utp_hash_table_find(&table, hash_key((uint32_t)index),
                               &items[index].key, hash_matches, NULL);
    assert(node == &items[index].node);
  }
  utp_hash_iter_init(&iter);
  while (utp_hash_iter_next(&table, &iter) != NULL) {
    ++seen;
  }
  assert(seen == ITEM_COUNT);
  for (index = 0; index < ITEM_COUNT; index += 2u) {
    assert(utp_hash_table_remove(&table, &items[index].node) ==
           UTP_INTERNAL_ERROR_OK);
    items[index].present = 0;
  }
  for (index = 0; index < ITEM_COUNT; ++index) {
    node = utp_hash_table_find(&table, hash_key(items[index].key),
                               &items[index].key, hash_matches, NULL);
    assert((node != NULL) == (items[index].present != 0));
  }
  utp_hash_table_cleanup(&table, NULL, NULL);
  assert(utp_hash_table_count(&table) == 0u);
}

static void test_hash_capacity_and_table_ownership(void) {
  utp_hash_table_t first;
  utp_hash_table_t second;
  hash_item_t items[3];
  size_t index;

  memset(items, 0, sizeof(items));
  assert(utp_hash_table_init(&first, NULL, 2u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_hash_table_init(&second, NULL, 2u) == UTP_INTERNAL_ERROR_OK);
  for (index = 0; index < 3u; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
  }
  assert(utp_hash_table_insert(&first, &items[0].node, hash_key(items[0].key),
                               &items[0].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_OK);
  assert(utp_hash_table_insert(&first, &items[1].node, hash_key(items[1].key),
                               &items[1].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_OK);
  assert(utp_hash_table_insert(&first, &items[2].node, hash_key(items[2].key),
                               &items[2].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_LIMIT);
  assert(utp_hash_table_insert(&second, &items[0].node, hash_key(items[0].key),
                               &items[0].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_STATE);
  assert(utp_hash_table_remove(&second, &items[0].node) ==
         UTP_INTERNAL_ERROR_NOT_FOUND);
  assert(utp_hash_table_count(&first) == 2u &&
         utp_hash_table_count(&second) == 0u);
  assert(utp_hash_table_remove(&first, &items[0].node) ==
         UTP_INTERNAL_ERROR_OK);
  assert(utp_hash_table_insert(&second, &items[0].node, hash_key(items[0].key),
                               &items[0].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_OK);
  utp_hash_table_cleanup(&first, NULL, NULL);
  utp_hash_table_cleanup(&second, NULL, NULL);
}

static void test_hash_initial_bucket_rounding(void) {
  fail_allocator_t failed_state = {0u, 0u};
  utp_allocator_t failed_allocator = make_fail_allocator(&failed_state);
  utp_hash_table_t table;

  assert(utp_hash_table_init_with_buckets(&table, NULL, 64u, 9u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(table.bucket_count == 16u && table.buckets != NULL);
  utp_hash_table_cleanup(&table, NULL, NULL);

  assert(utp_hash_table_init_with_buckets(&table, NULL, 64u, 1u) ==
         UTP_INTERNAL_ERROR_OK);
  assert(table.bucket_count == 1u && table.buckets != NULL);
  utp_hash_table_cleanup(&table, NULL, NULL);

  assert(utp_hash_table_init_with_buckets(&table, NULL, 64u, SIZE_MAX) ==
         UTP_INTERNAL_ERROR_OVERFLOW);
  assert(table.buckets == NULL && table.bucket_count == 0u &&
         table.allocator == NULL);

  assert(utp_hash_table_init_with_buckets(&table, &failed_allocator, 64u, 9u) ==
         UTP_INTERNAL_ERROR_NOMEM);
  assert(table.buckets == NULL && table.bucket_count == 0u &&
         table.allocator == NULL);
  utp_hash_table_cleanup(&table, NULL, NULL);
}

static void test_hash_cleanup_callback(void) {
  hash_item_t items[2];
  removal_state_t state = {0u};
  utp_hash_table_t table;
  size_t index;

  memset(items, 0, sizeof(items));
  assert(utp_hash_table_init(&table, NULL, 2u) == UTP_INTERNAL_ERROR_OK);
  for (index = 0; index < 2u; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
    assert(utp_hash_table_insert(&table, &items[index].node,
                                 hash_key(items[index].key), &items[index].key,
                                 hash_matches, NULL) == UTP_INTERNAL_ERROR_OK);
  }
  utp_hash_table_cleanup(&table, count_removed_node, &state);
  assert(state.count == 2u && utp_hash_table_count(&table) == 0u);
  utp_hash_table_cleanup(&table, count_removed_node, &state);
  assert(state.count == 2u);
}

static void test_hash_random(void) {
  enum { ITEM_COUNT = 256, OPERATIONS = 100000 };
  hash_item_t items[ITEM_COUNT];
  utp_hash_table_t table;
  uint32_t state = UINT32_C(0x8c4f2179);
  size_t operation;
  size_t index;

  memset(items, 0, sizeof(items));
  assert(utp_hash_table_init(&table, NULL, ITEM_COUNT) ==
         UTP_INTERNAL_ERROR_OK);
  for (index = 0; index < ITEM_COUNT; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
  }
  for (operation = 0; operation < OPERATIONS; ++operation) {
    uint32_t key = random_next(&state) % ITEM_COUNT;
    uint32_t action = random_next(&state) % 3u;
    utp_hash_node_t *found =
        utp_hash_table_find(&table, hash_key(key), &key, hash_matches, NULL);
    if (action == 0u) {
      utp_internal_error_t status = utp_hash_table_insert(
          &table, &items[key].node, hash_key(key), &key, hash_matches, NULL);
      assert(status == (items[key].present ? UTP_INTERNAL_ERROR_STATE
                                           : UTP_INTERNAL_ERROR_OK));
      if (status == UTP_INTERNAL_ERROR_OK) {
        items[key].present = 1;
      }
    } else if (action == 1u) {
      utp_internal_error_t status =
          utp_hash_table_remove(&table, &items[key].node);
      assert(status == (items[key].present ? UTP_INTERNAL_ERROR_OK
                                           : UTP_INTERNAL_ERROR_NOT_FOUND));
      if (status == UTP_INTERNAL_ERROR_OK) {
        items[key].present = 0;
      }
    } else {
      assert((found != NULL) == (items[key].present != 0));
    }
  }
  for (index = 0; index < ITEM_COUNT; ++index) {
    uint32_t key = (uint32_t)index;
    utp_hash_node_t *found =
        utp_hash_table_find(&table, hash_key(key), &key, hash_matches, NULL);
    assert((found != NULL) == (items[index].present != 0));
  }
  utp_hash_table_cleanup(&table, NULL, NULL);
}

static void test_hash_allocation_failure(void) {
  fail_allocator_t state = {0u, 0u};
  utp_allocator_t allocator = make_fail_allocator(&state);
  utp_hash_table_t table;
  hash_item_t item;
  uint32_t key = 7u;

  memset(&item, 0, sizeof(item));
  item.key = key;
  utp_hash_node_init(&item.node);
  assert(utp_hash_table_init(&table, &allocator, 4u) == UTP_INTERNAL_ERROR_OK);
  assert(utp_hash_table_insert(&table, &item.node, hash_key(key), &key,
                               hash_matches, NULL) == UTP_INTERNAL_ERROR_NOMEM);
  assert(table.count == 0u && table.buckets == NULL &&
         item.node.prev_next == NULL);
  state.calls = 0u;
  state.fail_after = 1u;
  assert(utp_hash_table_insert(&table, &item.node, hash_key(key), &key,
                               hash_matches, NULL) == UTP_INTERNAL_ERROR_OK);
  assert(table.count == 1u);
  utp_hash_table_cleanup(&table, NULL, NULL);
}

static void test_hash_rehash_allocation_failure(void) {
  enum { ITEM_COUNT = 7 };
  fail_allocator_t state = {0u, SIZE_MAX};
  utp_allocator_t allocator = make_fail_allocator(&state);
  hash_item_t items[ITEM_COUNT];
  utp_hash_table_t table;
  size_t index;

  memset(items, 0, sizeof(items));
  assert(utp_hash_table_init(&table, &allocator, 16u) == UTP_INTERNAL_ERROR_OK);
  for (index = 0; index < 6u; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
    assert(utp_hash_table_insert(&table, &items[index].node,
                                 hash_key(items[index].key), &items[index].key,
                                 hash_matches, NULL) == UTP_INTERNAL_ERROR_OK);
  }
  items[6].key = 6u;
  utp_hash_node_init(&items[6].node);
  assert(table.bucket_count == 8u && table.count == 6u);
  state.fail_after = state.calls;
  assert(utp_hash_table_insert(&table, &items[6].node, hash_key(items[6].key),
                               &items[6].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_NOMEM);
  assert(table.bucket_count == 8u && table.count == 6u &&
         items[6].node.table == NULL);
  for (index = 0; index < 6u; ++index) {
    uint32_t key = (uint32_t)index;
    assert(utp_hash_table_find(&table, hash_key(key), &key, hash_matches,
                               NULL) == &items[index].node);
  }
  state.fail_after = SIZE_MAX;
  assert(utp_hash_table_insert(&table, &items[6].node, hash_key(items[6].key),
                               &items[6].key, hash_matches,
                               NULL) == UTP_INTERNAL_ERROR_OK);
  assert(table.bucket_count == 16u && table.count == ITEM_COUNT);
  utp_hash_table_cleanup(&table, NULL, NULL);
}

int main(void) {
  test_version_and_status();
  test_internal_error_and_log_tag();
  test_buffer();
  test_buffer_allocation_failure();
  test_ring();
  test_ring_allocation_failure();
  test_range_set();
  test_range_set_random();
  test_range_set_allocation_failure();
  test_hash();
  test_hash_capacity_and_table_ownership();
  test_hash_initial_bucket_rounding();
  test_hash_cleanup_callback();
  test_hash_random();
  test_hash_allocation_failure();
  test_hash_rehash_allocation_failure();
  return 0;
}
