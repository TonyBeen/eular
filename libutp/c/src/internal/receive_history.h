#ifndef EULAR_UTP_INTERNAL_RECEIVE_HISTORY_H
#define EULAR_UTP_INTERNAL_RECEIVE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/allocator.h"
#include "internal/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_receive_range {
    uint64_t low;
    uint64_t high;
} utp_receive_range_t;

typedef struct utp_receive_history {
    utp_receive_range_t   *ranges;
    size_t                 range_count;
    size_t                 range_capacity;
    uint64_t               cutoff;
    uint64_t               largest;
    uint64_t               largest_received_at;
    const utp_allocator_t *allocator;
} utp_receive_history_t;

utp_internal_error_t       utp_receive_history_init(utp_receive_history_t *history, const utp_allocator_t *allocator,
                                                    size_t range_capacity);
void                       utp_receive_history_cleanup(utp_receive_history_t *history);
utp_internal_error_t       utp_receive_history_insert(utp_receive_history_t *history, uint64_t packet_number,
                                                      uint64_t received_at);
bool                       utp_receive_history_contains(const utp_receive_history_t *history, uint64_t packet_number);
utp_internal_error_t       utp_receive_history_stop_wait(utp_receive_history_t *history, uint64_t cutoff);
void                       utp_receive_history_clear(utp_receive_history_t *history);
uint64_t                   utp_receive_history_largest(const utp_receive_history_t *history);
uint64_t                   utp_receive_history_largest_received_at(const utp_receive_history_t *history);
uint64_t                   utp_receive_history_cutoff(const utp_receive_history_t *history);
size_t                     utp_receive_history_range_count(const utp_receive_history_t *history);
const utp_receive_range_t *utp_receive_history_range_at(const utp_receive_history_t *history, size_t index);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_RECEIVE_HISTORY_H
