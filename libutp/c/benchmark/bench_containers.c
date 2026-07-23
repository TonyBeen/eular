#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <utp/utp.h>

#include "internal/hash.h"
#include "internal/range_set.h"

typedef struct benchmark_item {
  utp_hash_node_t node;
  uint32_t key;
} benchmark_item_t;

static uint64_t hash_key(uint32_t key) {
  uint64_t value = key;

  value ^= value >> 16u;
  value *= UINT64_C(0x7feb352d);
  value ^= value >> 15u;
  value *= UINT64_C(0x846ca68b);
  value ^= value >> 16u;
  return value;
}

static benchmark_item_t *item_from_node(const utp_hash_node_t *node) {
  return (benchmark_item_t *)((char *)node - offsetof(benchmark_item_t, node));
}

static bool hash_matches(const utp_hash_node_t *node, const void *key,
                         void *user_data) {
  const uint32_t *expected = key;
  const benchmark_item_t *item = item_from_node(node);

  (void)user_data;
  return item->key == *expected;
}

static uint32_t random_next(uint32_t *state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

static double elapsed_seconds(clock_t start, clock_t end) {
  return (double)(end - start) / (double)CLOCKS_PER_SEC;
}

static void print_rate(const char *name, size_t operations, double seconds) {
  const double rate = seconds > 0.0 ? (double)operations / seconds : 0.0;

  printf("%-20s %10zu ops  %8.4f s  %12.0f ops/s\n", name, operations, seconds,
         rate);
}

static int parse_operations(int argc, char **argv, size_t *operations) {
  char *end;
  unsigned long long parsed;

  *operations = 1000000u;
  if (argc == 1) {
    return 1;
  }
  if (argc != 2) {
    return 0;
  }
  errno = 0;
  parsed = strtoull(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0u ||
      parsed > SIZE_MAX) {
    return 0;
  }
  *operations = (size_t)parsed;
  return 1;
}

int main(int argc, char **argv) {
  enum { ITEM_COUNT = 262144 };
  const uint32_t seed = UINT32_C(0x71d904a5);
  benchmark_item_t *items;
  utp_hash_table_t table;
  utp_range_set_t ranges;
  size_t operations;
  size_t index;
  uint32_t state = seed;
  volatile uint64_t checksum = 0u;
  clock_t start;
  clock_t end;

  if (!parse_operations(argc, argv, &operations)) {
    fprintf(stderr, "usage: %s [operations]\n", argv[0]);
    return EXIT_FAILURE;
  }
  items = calloc(ITEM_COUNT, sizeof(*items));
  if (items == NULL) {
    fputs("unable to allocate benchmark items\n", stderr);
    return EXIT_FAILURE;
  }
  if (utp_hash_table_init(&table, NULL, ITEM_COUNT) != UTP_INTERNAL_ERROR_OK ||
      utp_range_set_init(&ranges, NULL, ITEM_COUNT) != UTP_INTERNAL_ERROR_OK) {
    fputs("unable to initialize benchmark containers\n", stderr);
    free(items);
    return EXIT_FAILURE;
  }
  for (index = 0; index < ITEM_COUNT; ++index) {
    items[index].key = (uint32_t)index;
    utp_hash_node_init(&items[index].node);
  }

  printf("libutp-c container benchmark: seed=0x%08" PRIx32 ", items=%u\n", seed,
         (unsigned int)ITEM_COUNT);
  start = clock();
  for (index = 0; index < ITEM_COUNT; ++index) {
    if (utp_hash_table_insert(&table, &items[index].node,
                              hash_key(items[index].key), &items[index].key,
                              hash_matches, NULL) != UTP_INTERNAL_ERROR_OK) {
      fputs("hash insert failed\n", stderr);
      utp_hash_table_cleanup(&table, NULL, NULL);
      utp_range_set_cleanup(&ranges);
      free(items);
      return EXIT_FAILURE;
    }
  }
  end = clock();
  print_rate("hash insert", ITEM_COUNT, elapsed_seconds(start, end));

  start = clock();
  for (index = 0; index < operations; ++index) {
    uint32_t key = random_next(&state) % ITEM_COUNT;
    utp_hash_node_t *node =
        utp_hash_table_find(&table, hash_key(key), &key, hash_matches, NULL);

    if (node == NULL) {
      fputs("hash lookup failed\n", stderr);
      utp_hash_table_cleanup(&table, NULL, NULL);
      utp_range_set_cleanup(&ranges);
      free(items);
      return EXIT_FAILURE;
    }
    checksum += item_from_node(node)->key;
  }
  end = clock();
  print_rate("hash lookup", operations, elapsed_seconds(start, end));

  start = clock();
  for (index = 0; index < ITEM_COUNT; ++index) {
    if (utp_hash_table_remove(&table, &items[index].node) !=
        UTP_INTERNAL_ERROR_OK) {
      fputs("hash remove failed\n", stderr);
      utp_hash_table_cleanup(&table, NULL, NULL);
      utp_range_set_cleanup(&ranges);
      free(items);
      return EXIT_FAILURE;
    }
  }
  end = clock();
  print_rate("hash remove", ITEM_COUNT, elapsed_seconds(start, end));

  state = seed;
  start = clock();
  for (index = 0; index < operations; ++index) {
    uint64_t first = random_next(&state) % ITEM_COUNT;
    uint64_t length = (random_next(&state) % 64u) + 1u;

    if (utp_range_set_insert(&ranges, first, first + length) !=
        UTP_INTERNAL_ERROR_OK) {
      fputs("range insert failed\n", stderr);
      utp_hash_table_cleanup(&table, NULL, NULL);
      utp_range_set_cleanup(&ranges);
      free(items);
      return EXIT_FAILURE;
    }
    checksum += ranges.count;
  }
  end = clock();
  print_rate("range merge", operations, elapsed_seconds(start, end));
  printf("range_count=%zu checksum=%" PRIu64 "\n", ranges.count, checksum);

  utp_hash_table_cleanup(&table, NULL, NULL);
  utp_range_set_cleanup(&ranges);
  free(items);
  return EXIT_SUCCESS;
}
