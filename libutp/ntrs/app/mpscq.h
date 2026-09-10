#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

// Dmitry Vyukov's bounded MPMC queue, used here with one consumer.
// Source: https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
//
// Storage is allocated once before producers start. push returns false when the
// queue is full; pop returns false when it is empty or a producer is still
// publishing the next slot.
template <typename T>
class MpscQueue
{
    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

public:
    MpscQueue() : slots_(), capacity_(0u), enqueue_position_(0u), dequeue_position_(0u) {}

    bool create(size_t capacity)
    {
        if (capacity == 0u || slots_) return false;
        std::unique_ptr<Slot[]> slots(new (std::nothrow) Slot[capacity]);

        if (!slots) return false;
        for (size_t index = 0u; index < capacity; ++index)
            slots[index].sequence.store(index, std::memory_order_relaxed);
        slots_    = std::move(slots);
        capacity_ = capacity;
        enqueue_position_.store(0u, std::memory_order_relaxed);
        dequeue_position_ = 0u;
        return true;
    }

    bool push(const T& value)
    {
        size_t position;

        if (!slots_) return false;
        position = enqueue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Slot&          slot       = slots_[position % capacity_];
            const size_t   sequence   = slot.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);

            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(position, position + 1u, std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
                    slot.value = value;
                    slot.sequence.store(position + 1u, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T* value)
    {
        if (!slots_ || value == NULL) return false;
        Slot&          slot       = slots_[dequeue_position_ % capacity_];
        const size_t   sequence   = slot.sequence.load(std::memory_order_acquire);
        const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(dequeue_position_ + 1u);

        if (difference != 0) return false;
        *value = slot.value;
        slot.sequence.store(dequeue_position_ + capacity_, std::memory_order_release);
        ++dequeue_position_;
        return true;
    }

private:
    struct Slot {
        std::atomic<size_t> sequence;
        T                   value;

        Slot() : sequence(0u), value() {}
    };

    std::unique_ptr<Slot[]> slots_;
    size_t                  capacity_;
    std::atomic<size_t>     enqueue_position_;
    size_t                  dequeue_position_;
};
