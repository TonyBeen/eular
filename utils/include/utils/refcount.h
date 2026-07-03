/*************************************************************************
    > File Name: refcount.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 04:48:18 PM CST
 ************************************************************************/

#ifndef __UTILS_REF_COUNT_H__
#define __UTILS_REF_COUNT_H__

#include <atomic>

namespace eular {
class RefCount
{
public:
    RefCount() noexcept {}
    RefCount(uint32_t init) noexcept : atomic_ref_count(init) {}
    ~RefCount() noexcept {}

    RefCount(const RefCount& other) = delete;
    RefCount& operator=(const RefCount& other) = delete;

    inline void ref() noexcept { ++atomic_ref_count; }

    inline uint32_t deref() noexcept
    {
        if (atomic_ref_count.load() == 0) {
            return 0;
        }

        return --atomic_ref_count;
    }

    inline uint32_t load() const noexcept { return atomic_ref_count; }

    std::atomic<uint32_t> atomic_ref_count{0};
};

}  // namespace eular

#endif  // __UTILS_REF_COUNT_H__
