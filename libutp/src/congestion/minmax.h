/*************************************************************************
    > File Name: minmax.h
    > Author: eular
    > Brief:
    > Created Time: Wed 10 Dec 2025 12:01:31 PM CST
 ************************************************************************/

#ifndef __CONGESTION_MINMAX_H__
#define __CONGESTION_MINMAX_H__

#include <stdint.h>

#define MINMAX_SAMPLES    3

namespace eular {
namespace utp {
struct MinMaxSample {
    uint64_t    time;
    uint32_t    value;
};

struct MinMax {
    uint64_t        window;
    MinMaxSample    samples[MINMAX_SAMPLES];

    void init(uint64_t win);

    uint64_t get(int32_t idx) const;
    uint64_t get() const {
        return get(0);
    }

    void updateMin(uint64_t now, uint64_t meas);
    void updateMax(uint64_t now, uint64_t meas);

    void reset(MinMaxSample init);
private:
    void subwinUpdate(const MinMaxSample *sample);
};

} // namespace utp
} // namespace eular

#endif // __CONGESTION_MINMAX_H__
