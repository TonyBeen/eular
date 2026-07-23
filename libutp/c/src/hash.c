#include "internal/hash.h"

#include <limits.h>
#include <string.h>

#define UTP_HASH_INITIAL_BUCKETS 8u

static bool is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1u)) == 0;
}

static utp_internal_error_t normalize_bucket_count(size_t requested,
                                                   size_t *bucket_count) {
  size_t normalized = 1u;

  if (bucket_count == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (requested == 0u) {
    *bucket_count = 0u;
    return UTP_INTERNAL_ERROR_OK;
  }
  while (normalized < requested) {
    if (normalized > SIZE_MAX / 2u) {
      return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    normalized *= 2u;
  }
  if (normalized > SIZE_MAX / sizeof(utp_hash_node_t *)) {
    return UTP_INTERNAL_ERROR_OVERFLOW;
  }
  *bucket_count = normalized;
  return UTP_INTERNAL_ERROR_OK;
}

static size_t bucket_index(const utp_hash_table_t *table, uint64_t hash) {
  return (size_t)hash & (table->bucket_count - 1u);
}

static void link_node(utp_hash_node_t **head, utp_hash_node_t *node) {
  node->next = *head;
  node->prev_next = head;
  if (*head != NULL) {
    (*head)->prev_next = &node->next;
  }
  *head = node;
}

static void unlink_node(utp_hash_node_t *node) {
  if (node->prev_next == NULL) {
    return;
  }
  *node->prev_next = node->next;
  if (node->next != NULL) {
    node->next->prev_next = node->prev_next;
  }
  node->next = NULL;
  node->prev_next = NULL;
  node->table = NULL;
}

static utp_internal_error_t rehash(utp_hash_table_t *table,
                                   size_t bucket_count) {
  utp_hash_node_t **new_buckets;
  size_t index;

  if (!is_power_of_two(bucket_count) ||
      bucket_count > SIZE_MAX / sizeof(*new_buckets)) {
    return UTP_INTERNAL_ERROR_OVERFLOW;
  }
  new_buckets = utp_allocator_alloc(table->allocator,
                                    bucket_count * sizeof(*new_buckets));
  if (new_buckets == NULL) {
    return UTP_INTERNAL_ERROR_NOMEM;
  }
  memset(new_buckets, 0, bucket_count * sizeof(*new_buckets));
  for (index = 0; index < table->bucket_count; ++index) {
    utp_hash_node_t *node = table->buckets[index];
    while (node != NULL) {
      utp_hash_node_t *next = node->next;
      node->next = NULL;
      node->prev_next = NULL;
      link_node(&new_buckets[(size_t)node->hash & (bucket_count - 1u)], node);
      node = next;
    }
  }
  utp_allocator_free(table->allocator, table->buckets);
  table->buckets = new_buckets;
  table->bucket_count = bucket_count;
  return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t ensure_insert_capacity(utp_hash_table_t *table) {
  size_t next;

  if (table->count >= table->max_entries) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  if (table->bucket_count == 0) {
    return rehash(table, UTP_HASH_INITIAL_BUCKETS);
  }
  if (table->count + 1u <= table->bucket_count - table->bucket_count / 4u) {
    return UTP_INTERNAL_ERROR_OK;
  }
  if (table->bucket_count > SIZE_MAX / 2u) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  next = table->bucket_count * 2u;
  return rehash(table, next);
}

utp_internal_error_t utp_hash_table_init(utp_hash_table_t *table,
                                         const utp_allocator_t *allocator,
                                         size_t max_entries) {
  return utp_hash_table_init_with_buckets(table, allocator, max_entries, 0u);
}

utp_internal_error_t utp_hash_table_init_with_buckets(
    utp_hash_table_t *table, const utp_allocator_t *allocator,
    size_t max_entries, size_t initial_bucket_count) {
  const utp_allocator_t *resolved;
  size_t bucket_count;
  utp_internal_error_t status;

  if (table == NULL || max_entries == 0) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  resolved = utp_allocator_resolve(allocator);
  if (resolved == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  memset(table, 0, sizeof(*table));
  table->max_entries = max_entries;
  table->allocator = resolved;
  status = normalize_bucket_count(initial_bucket_count, &bucket_count);
  if (status != UTP_INTERNAL_ERROR_OK) {
    memset(table, 0, sizeof(*table));
    return status;
  }
  if (bucket_count == 0u) {
    return UTP_INTERNAL_ERROR_OK;
  }
  status = rehash(table, bucket_count);
  if (status != UTP_INTERNAL_ERROR_OK) {
    memset(table, 0, sizeof(*table));
    return status;
  }
  return UTP_INTERNAL_ERROR_OK;
}

void utp_hash_node_init(utp_hash_node_t *node) {
  if (node != NULL) {
    memset(node, 0, sizeof(*node));
  }
}

void utp_hash_table_clear(utp_hash_table_t *table, utp_hash_node_fn on_remove,
                          void *user_data) {
  size_t index;

  if (table == NULL) {
    return;
  }
  for (index = 0; index < table->bucket_count; ++index) {
    utp_hash_node_t *node = table->buckets[index];
    table->buckets[index] = NULL;
    while (node != NULL) {
      utp_hash_node_t *next = node->next;
      node->next = NULL;
      node->prev_next = NULL;
      node->table = NULL;
      if (on_remove != NULL) {
        on_remove(node, user_data);
      }
      node = next;
    }
  }
  table->count = 0;
}

void utp_hash_table_cleanup(utp_hash_table_t *table, utp_hash_node_fn on_remove,
                            void *user_data) {
  if (table == NULL) {
    return;
  }
  utp_hash_table_clear(table, on_remove, user_data);
  utp_allocator_free(table->allocator, table->buckets);
  memset(table, 0, sizeof(*table));
}

size_t utp_hash_table_count(const utp_hash_table_t *table) {
  return table == NULL ? 0 : table->count;
}

utp_hash_node_t *utp_hash_table_find(const utp_hash_table_t *table,
                                     uint64_t hash, const void *key,
                                     utp_hash_match_fn matches,
                                     void *user_data) {
  utp_hash_node_t *node;

  if (table == NULL || matches == NULL || table->bucket_count == 0) {
    return NULL;
  }
  for (node = table->buckets[bucket_index(table, hash)]; node != NULL;
       node = node->next) {
    if (node->hash == hash && matches(node, key, user_data)) {
      return node;
    }
  }
  return NULL;
}

utp_internal_error_t utp_hash_table_insert(utp_hash_table_t *table,
                                           utp_hash_node_t *node, uint64_t hash,
                                           const void *key,
                                           utp_hash_match_fn matches,
                                           void *user_data) {
  utp_internal_error_t status;

  if (table == NULL || node == NULL || matches == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (node->prev_next != NULL || node->table != NULL) {
    return UTP_INTERNAL_ERROR_STATE;
  }
  if (utp_hash_table_find(table, hash, key, matches, user_data) != NULL) {
    return UTP_INTERNAL_ERROR_EXISTS;
  }
  status = ensure_insert_capacity(table);
  if (status != UTP_INTERNAL_ERROR_OK) {
    return status;
  }
  node->hash = hash;
  link_node(&table->buckets[bucket_index(table, hash)], node);
  node->table = table;
  ++table->count;
  return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_hash_table_remove(utp_hash_table_t *table,
                                           utp_hash_node_t *node) {
  if (table == NULL || node == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (node->table != table || node->prev_next == NULL) {
    return UTP_INTERNAL_ERROR_NOT_FOUND;
  }
  unlink_node(node);
  --table->count;
  return UTP_INTERNAL_ERROR_OK;
}

void utp_hash_iter_init(utp_hash_iter_t *iter) {
  if (iter != NULL) {
    memset(iter, 0, sizeof(*iter));
  }
}

utp_hash_node_t *utp_hash_iter_next(const utp_hash_table_t *table,
                                    utp_hash_iter_t *iter) {
  utp_hash_node_t *result;

  if (table == NULL || iter == NULL) {
    return NULL;
  }
  if (iter->next != NULL) {
    result = iter->next;
    iter->next = result->next;
    return result;
  }
  while (iter->bucket_index < table->bucket_count) {
    utp_hash_node_t *head = table->buckets[iter->bucket_index++];
    if (head != NULL) {
      iter->next = head->next;
      return head;
    }
  }
  return NULL;
}
