// Windowed min/max tracker based on the three-sample algorithm used by lsquic.

#include "congestion/minmax.h"

#include <assert.h>
#include <stddef.h>

static void utp_minmax_reset(utp_minmax_t *minmax, utp_minmax_sample_t sample) {
    assert(minmax != NULL);
    minmax->samples[0] = sample;
    minmax->samples[1] = sample;
    minmax->samples[2] = sample;
}

static void utp_minmax_subwindow_update(utp_minmax_t *minmax, const utp_minmax_sample_t *sample) {
    assert(minmax != NULL);
    assert(sample != NULL);
    uint64_t elapsed = sample->time - minmax->samples[0].time;

    if (elapsed > minmax->window) {
        minmax->samples[0] = minmax->samples[1];
        minmax->samples[1] = minmax->samples[2];
        minmax->samples[2] = *sample;
        if (sample->time - minmax->samples[0].time > minmax->window) {
            minmax->samples[0] = minmax->samples[1];
            minmax->samples[1] = minmax->samples[2];
            minmax->samples[2] = *sample;
        }
    } else if (minmax->samples[1].time == minmax->samples[0].time && elapsed > minmax->window / 4u) {
        minmax->samples[2] = *sample;
        minmax->samples[1] = *sample;
    } else if (minmax->samples[2].time == minmax->samples[1].time && elapsed > minmax->window / 2u) {
        minmax->samples[2] = *sample;
    }
}

void utp_minmax_init(utp_minmax_t *minmax, uint64_t window) {
    assert(minmax != NULL);
    minmax->window           = window;
    minmax->samples[0].time  = 0u;
    minmax->samples[0].value = 0u;
    minmax->samples[1].time  = 0u;
    minmax->samples[1].value = 0u;
    minmax->samples[2].time  = 0u;
    minmax->samples[2].value = 0u;
}

uint64_t utp_minmax_get(const utp_minmax_t *minmax) { return utp_minmax_get_at(minmax, 0u); }

uint64_t utp_minmax_get_at(const utp_minmax_t *minmax, uint32_t index) {
    assert(minmax != NULL);
    return minmax->samples[index < UTP_MINMAX_SAMPLE_COUNT ? index : 0u].value;
}

void utp_minmax_update_min(utp_minmax_t *minmax, uint64_t now, uint64_t measurement) {
    utp_minmax_sample_t sample = {now, measurement};

    assert(minmax != NULL);
    if (minmax->samples[0].value == 0u || sample.value <= minmax->samples[0].value ||
        sample.time - minmax->samples[2].time > minmax->window) {
        utp_minmax_reset(minmax, sample);
        return;
    }
    if (sample.value <= minmax->samples[1].value) {
        minmax->samples[2] = sample;
        minmax->samples[1] = sample;
    } else if (sample.value <= minmax->samples[2].value) {
        minmax->samples[2] = sample;
    }
    utp_minmax_subwindow_update(minmax, &sample);
}

void utp_minmax_update_max(utp_minmax_t *minmax, uint64_t now, uint64_t measurement) {
    utp_minmax_sample_t sample = {now, measurement};

    assert(minmax != NULL);
    if (minmax->samples[0].value == 0u || sample.value >= minmax->samples[0].value ||
        sample.time - minmax->samples[2].time > minmax->window) {
        utp_minmax_reset(minmax, sample);
        return;
    }
    if (sample.value >= minmax->samples[1].value) {
        minmax->samples[2] = sample;
        minmax->samples[1] = sample;
    } else if (sample.value >= minmax->samples[2].value) {
        minmax->samples[2] = sample;
    }
    utp_minmax_subwindow_update(minmax, &sample);
}
