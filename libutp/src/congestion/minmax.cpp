/*************************************************************************
    > File Name: minmax.cpp
    > Author: hsz
    > Brief:
    > Created Time: Wed 10 Dec 2025 12:01:34 PM CST
 ************************************************************************/

#include "congestion/minmax.h"

#include <string.h>
#include "minmax.h"

namespace eular {
namespace utp {
void MinMax::init(uint64_t win)
{
    window = win;
    reset(MinMaxSample{0, 0});
}

uint64_t MinMax::get(int32_t idx) const
{
    if (idx < 0 || idx >= MINMAX_SAMPLES) {
        return samples[0].value;
    }

    return samples[idx].value;
}

void MinMax::updateMin(uint64_t now, uint64_t meas)
{
    MinMaxSample sample{now, meas};
    if (samples[0].value == 0 ||                    // uninitialized
        sample.value <= samples[0].value ||         // found new min?
        sample.time - samples[2].time > window) {   // nothing left in window?
        reset(sample);
        return;
    }

    if (sample.value <= samples[1].value) {
        samples[2] = samples[1] = sample;
    } else if (sample.value <= samples[2].value) {
        samples[2] = sample;
    }

    subwinUpdate(&sample);
}

void MinMax::updateMax(uint64_t now, uint64_t meas)
{
    MinMaxSample sample{now, meas};
    if (samples[0].value == 0 ||                    // uninitialized
        sample.value >= samples[0].value ||         // found new max?
        sample.time - samples[2].time > window) {   // nothing left in window?
        reset(sample);
        return;
    }

    if (sample.value >= samples[1].value) {
        samples[2] = samples[1] = sample;
    } else if (sample.value >= samples[2].value) {
        samples[2] = sample;
    }

    subwinUpdate(&sample);
}

void MinMax::reset(MinMaxSample init)
{
    for (int32_t i = 0; i < MINMAX_SAMPLES; ++i) {
        samples[i] = init;
    }
}

void MinMax::subwinUpdate(const MinMaxSample *sample)
{
    uint64_t dt = sample->time - samples[0].time;

    if (dt > window)
    {
        /*
         * Passed entire window without a new sample so make 2nd
         * choice the new sample & 3rd choice the new 2nd choice.
         * we may have to iterate this since our 2nd choice
         * may also be outside the window (we checked on entry
         * that the third choice was in the window).
         */
        samples[0] = samples[1];
        samples[1] = samples[2];
        samples[2] = *sample;
        if (sample->time - samples[0].time > window) {
            samples[0] = samples[1];
            samples[1] = samples[2];
            samples[2] = *sample;
        }
    }
    else if (samples[1].time == samples[0].time
                                                && dt > window / 4)
    {
        /*
         * We've passed a quarter of the window without a new sample
         * so take a 2nd choice from the 2nd quarter of the window.
         */
        samples[2] = samples[1] = *sample;
    }
    else if (samples[2].time == samples[1].time
                                                && dt > window / 2)
    {
        /*
         * We've passed half the window without finding a new sample
         * so take a 3rd choice from the last half of the window
         */
        samples[2] = *sample;
    }
}

} // namespace utp
} // namespace eular
