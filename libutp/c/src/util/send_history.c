#include "util/send_history.h"

#include "proto/proto.h"

void utp_send_history_init(utp_send_history_t *history, uint64_t gap_warning_threshold) {
    if (history != NULL) {
        history->largest               = 0u;
        history->gap_warning_threshold = gap_warning_threshold;
        history->gap_detected          = false;
    }
}

utp_internal_error_t utp_send_history_update(utp_send_history_t *history, uint64_t packet_number) {
    if (history == NULL || packet_number == 0u || packet_number > UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (history->largest != packet_number - 1u && !history->gap_detected &&
        packet_number > history->gap_warning_threshold) {
        history->gap_detected = true;
    }
    if (packet_number > history->largest) {
        history->largest = packet_number;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_send_history_largest(const utp_send_history_t *history) { return history == NULL ? 0u : history->largest; }

bool utp_send_history_gap_detected(const utp_send_history_t *history) {
    return history != NULL && history->gap_detected;
}
