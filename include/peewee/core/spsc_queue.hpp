#pragma once
// Fixed-capacity lock-free Single Producer / Single Consumer queue.
//
// Transfers events between a venue's network thread (producer)
// and the engine thread (consumer). No mutex: one thread writes only
// tail_, the other writes only head_.
//
// For production, consider rigtorp::SPSCQueue, which is more optimized
// (cached indices and more careful padding).

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace peewee {

template <class T>
class SpscQueue {
public:
    // Round capacity up to the next power of two.
    explicit SpscQueue(size_t capacity = 1 << 16)
        : mask_(round_up_pow2(capacity) - 1),
          slots_(std::make_unique<std::optional<T>[]>(mask_ + 1)) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Called ONLY by the producer. Returns false if the queue is full.
    bool try_push(T&& value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) > mask_) return false;
        slots_[tail & mask_].emplace(std::move(value));
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Called ONLY by the consumer. Returns false if the queue is empty.
    bool try_pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        auto& slot = slots_[head & mask_];
        out = std::move(*slot);
        slot.reset();
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // Separate cache lines prevent false sharing between the two threads.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) const size_t mask_;
    std::unique_ptr<std::optional<T>[]> slots_;
};

}  // namespace peewee
