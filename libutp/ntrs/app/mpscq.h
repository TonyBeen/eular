#pragma once

#include <atomic>
#include <cstddef>

// Dmitry Vyukov intrusive MPSC node-based queue.
// Source: https://www.1024cores.net/home/lock-free-algorithms/queues
//         https://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
//
// Concurrency contract:
// - push: safe for multiple producer threads.
// - try_pop: safe for exactly one consumer thread.
//
// A null result can mean either an empty queue or the brief interval after a
// producer exchanged head and before it linked the previous node. Consumers
// must wait for their external notification and retry later.

struct mpscq_node_t {
    std::atomic<mpscq_node_t*> next;

    mpscq_node_t() : next(NULL) {}
    mpscq_node_t(const mpscq_node_t&)            = delete;
    mpscq_node_t& operator=(const mpscq_node_t&) = delete;
    mpscq_node_t(mpscq_node_t&&)                 = delete;
    mpscq_node_t& operator=(mpscq_node_t&&)      = delete;
};

struct mpscq_t {
    std::atomic<mpscq_node_t*> head;
    mpscq_node_t*              tail;
    mpscq_node_t               stub;

    mpscq_t() : head(&stub), tail(&stub), stub() {}
    mpscq_t(const mpscq_t&)            = delete;
    mpscq_t& operator=(const mpscq_t&) = delete;
    mpscq_t(mpscq_t&&)                 = delete;
    mpscq_t& operator=(mpscq_t&&)      = delete;
};

#ifndef container_of
#define container_of(ptr, type, member) reinterpret_cast<type*>(reinterpret_cast<char*>(ptr) - offsetof(type, member))
#endif

#ifndef mpscq_entry
#define mpscq_entry(ptr, type, member) container_of(ptr, type, member)
#endif

inline void mpscq_create(mpscq_t* self)
{
    self->stub.next.store(NULL, std::memory_order_relaxed);
    self->head.store(&self->stub, std::memory_order_relaxed);
    self->tail = &self->stub;
}

inline void mpscq_push(mpscq_t* self, mpscq_node_t* node)
{
    node->next.store(NULL, std::memory_order_relaxed);
    mpscq_node_t* const previous = self->head.exchange(node, std::memory_order_acq_rel);

    previous->next.store(node, std::memory_order_release);
}

inline mpscq_node_t* mpscq_try_pop(mpscq_t* self)
{
    mpscq_node_t* tail = self->tail;
    mpscq_node_t* next = tail->next.load(std::memory_order_acquire);

    if (tail == &self->stub) {
        if (next == NULL) return NULL;
        self->tail = next;
        tail       = next;
        next       = next->next.load(std::memory_order_acquire);
    }
    if (next != NULL) {
        self->tail = next;
        return tail;
    }
    if (tail != self->head.load(std::memory_order_acquire)) return NULL;

    mpscq_push(self, &self->stub);
    next = tail->next.load(std::memory_order_acquire);
    if (next == NULL) return NULL;
    self->tail = next;
    return tail;
}

inline mpscq_node_t* mpscq_pop(mpscq_t* self) { return mpscq_try_pop(self); }
