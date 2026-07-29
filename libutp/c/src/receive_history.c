#include "internal/receive_history.h"

#include <limits.h>
#include <string.h>

#include "internal/proto.h"

utp_internal_error_t utp_receive_history_init(utp_receive_history_t *history, const utp_allocator_t *allocator,
                                              size_t range_capacity) {
    const utp_allocator_t *resolved_allocator;

    if (history == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(history, 0, sizeof(*history));
    if (range_capacity == 0u || range_capacity > SIZE_MAX / sizeof(*history->ranges)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    history->ranges = utp_allocator_alloc(resolved_allocator, range_capacity * sizeof(*history->ranges));
    if (history->ranges == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    history->range_capacity = range_capacity;
    history->allocator      = resolved_allocator;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_receive_history_cleanup(utp_receive_history_t *history) {
    if (history == NULL) {
        return;
    }
    utp_allocator_free(history->allocator, history->ranges);
    memset(history, 0, sizeof(*history));
}

utp_internal_error_t utp_receive_history_insert(utp_receive_history_t *history, uint64_t packet_number,
                                                uint64_t received_at) {
    bool   extends_higher_range;
    bool   extends_lower_range;
    size_t position;

    if (history == NULL || history->ranges == NULL || packet_number == 0u || packet_number > UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (history->cutoff != 0u && packet_number < history->cutoff) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (packet_number > history->largest) {
        history->largest             = packet_number;
        history->largest_received_at = received_at;
    }

    position = 0u;
    while (position < history->range_count) {
        if (packet_number > history->ranges[position].high) {
            break;
        }
        if (packet_number >= history->ranges[position].low) {
            return UTP_INTERNAL_ERROR_OK;
        }
        ++position;
    }

    extends_higher_range = position > 0u && history->ranges[position - 1u].low == packet_number + 1u;
    extends_lower_range  = position < history->range_count && history->ranges[position].high + 1u == packet_number;
    if (extends_higher_range && extends_lower_range) {
        history->ranges[position - 1u].low = history->ranges[position].low;
        memmove(&history->ranges[position], &history->ranges[position + 1u],
                (history->range_count - position - 1u) * sizeof(*history->ranges));
        --history->range_count;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (extends_higher_range) {
        history->ranges[position - 1u].low = packet_number;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (extends_lower_range) {
        history->ranges[position].high = packet_number;
        return UTP_INTERNAL_ERROR_OK;
    }

    if (history->range_count == history->range_capacity) {
        if (position == history->range_count) {
            history->cutoff = packet_number + 1u;
            return UTP_INTERNAL_ERROR_OK;
        }
        history->cutoff = history->ranges[history->range_count - 1u].high + 1u;
        if (position + 1u < history->range_count) {
            memmove(&history->ranges[position + 1u], &history->ranges[position],
                    (history->range_count - position - 1u) * sizeof(*history->ranges));
        }
    } else {
        memmove(&history->ranges[position + 1u], &history->ranges[position],
                (history->range_count - position) * sizeof(*history->ranges));
        ++history->range_count;
    }
    history->ranges[position].low  = packet_number;
    history->ranges[position].high = packet_number;
    return UTP_INTERNAL_ERROR_OK;
}

bool utp_receive_history_contains(const utp_receive_history_t *history, uint64_t packet_number) {
    size_t index;

    if (history == NULL || packet_number == 0u) {
        return false;
    }
    if (history->cutoff != 0u && packet_number < history->cutoff) {
        return true;
    }
    for (index = 0u; index < history->range_count; ++index) {
        const utp_receive_range_t *range = &history->ranges[index];

        if (packet_number > range->high) {
            return false;
        }
        if (packet_number >= range->low) {
            return true;
        }
    }
    return false;
}

utp_internal_error_t utp_receive_history_stop_wait(utp_receive_history_t *history, uint64_t cutoff) {
    if (history == NULL || cutoff > UTP_PACKET_NUMBER_MAX + 1u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (cutoff <= history->cutoff) {
        return UTP_INTERNAL_ERROR_OK;
    }
    history->cutoff = cutoff;
    while (history->range_count != 0u) {
        utp_receive_range_t *oldest = &history->ranges[history->range_count - 1u];

        if (oldest->high < cutoff) {
            --history->range_count;
            continue;
        }
        if (oldest->low < cutoff) {
            oldest->low = cutoff;
        }
        break;
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_receive_history_clear(utp_receive_history_t *history) {
    if (history != NULL) {
        history->range_count         = 0u;
        history->cutoff              = 0u;
        history->largest             = 0u;
        history->largest_received_at = 0u;
    }
}

uint64_t utp_receive_history_largest(const utp_receive_history_t *history) {
    return history == NULL ? 0u : history->largest;
}

uint64_t utp_receive_history_largest_received_at(const utp_receive_history_t *history) {
    return history == NULL ? 0u : history->largest_received_at;
}

uint64_t utp_receive_history_cutoff(const utp_receive_history_t *history) {
    return history == NULL ? 0u : history->cutoff;
}

size_t utp_receive_history_range_count(const utp_receive_history_t *history) {
    return history == NULL ? 0u : history->range_count;
}

const utp_receive_range_t *utp_receive_history_range_at(const utp_receive_history_t *history, size_t index) {
    if (history == NULL || index >= history->range_count) {
        return NULL;
    }
    return &history->ranges[index];
}
