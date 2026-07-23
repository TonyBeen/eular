#ifndef EULAR_UTP_INTERNAL_HASH_H
#define EULAR_UTP_INTERNAL_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/allocator.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_hash_table utp_hash_table_t;

typedef struct utp_hash_node {
  struct utp_hash_node *next;
  struct utp_hash_node **prev_next;
  utp_hash_table_t *table;
  uint64_t hash;
} utp_hash_node_t;

typedef bool (*utp_hash_match_fn)(const utp_hash_node_t *node, const void *key,
                                  void *user_data);
typedef void (*utp_hash_node_fn)(utp_hash_node_t *node, void *user_data);

struct utp_hash_table {
  utp_hash_node_t **buckets;
  size_t bucket_count;
  size_t count;
  size_t max_entries;
  const utp_allocator_t *allocator;
};

typedef struct utp_hash_iter {
  size_t bucket_index;
  utp_hash_node_t *next;
} utp_hash_iter_t;

utp_internal_error_t utp_hash_table_init(utp_hash_table_t *table,
                                         const utp_allocator_t *allocator,
                                         size_t max_entries);
/*
 * Allocates an initial bucket array during initialization. A non-zero request
 * is rounded up to the nearest power of two. Pass zero to retain lazy bucket
 * allocation, as utp_hash_table_init() does.
 */
utp_internal_error_t utp_hash_table_init_with_buckets(
    utp_hash_table_t *table, const utp_allocator_t *allocator,
    size_t max_entries, size_t initial_bucket_count);
void utp_hash_node_init(utp_hash_node_t *node);
void utp_hash_table_clear(utp_hash_table_t *table, utp_hash_node_fn on_remove,
                          void *user_data);
void utp_hash_table_cleanup(utp_hash_table_t *table, utp_hash_node_fn on_remove,
                            void *user_data);
size_t utp_hash_table_count(const utp_hash_table_t *table);
utp_hash_node_t *utp_hash_table_find(const utp_hash_table_t *table,
                                     uint64_t hash, const void *key,
                                     utp_hash_match_fn matches,
                                     void *user_data);
utp_internal_error_t utp_hash_table_insert(utp_hash_table_t *table,
                                           utp_hash_node_t *node, uint64_t hash,
                                           const void *key,
                                           utp_hash_match_fn matches,
                                           void *user_data);
utp_internal_error_t utp_hash_table_remove(utp_hash_table_t *table,
                                           utp_hash_node_t *node);
void utp_hash_iter_init(utp_hash_iter_t *iter);
utp_hash_node_t *utp_hash_iter_next(const utp_hash_table_t *table,
                                    utp_hash_iter_t *iter);

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_INTERNAL_HASH_H */
