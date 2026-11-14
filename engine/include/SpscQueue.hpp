#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace exch {

inline constexpr std::size_t kCacheLine = 64;

// Lock-free ring buffer for exactly ONE producer thread and ONE consumer thread.
template <typename T, std::size_t N>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "items are copied as plain bytes");

public:
    SpscQueue() = default;

    static constexpr std::size_t capacity() { return N; }

    // Producer thread only. Returns false if the queue is full.
    bool tryPush(const T& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);   // my own counter
        const std::size_t head = head_.load(std::memory_order_acquire);   // consumer's progress
        if (tail - head == N) return false;                               // full
        buffer_[tail & (N - 1)] = item;
        tail_.store(tail + 1, std::memory_order_release);                 // publish the item
        return true;
    }

    // Consumer thread only. Returns false if the queue is empty.
    bool tryPop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);   // my own counter
        const std::size_t tail = tail_.load(std::memory_order_acquire);   // producer's progress
        if (head == tail) return false;                                   // empty
        out = buffer_[head & (N - 1)];
        head_.store(head + 1, std::memory_order_release);                 // free the slot
        return true;
    }

    // Approximate: the other thread may change it while we read. For metrics only.
    std::size_t sizeApprox() const {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // written by the consumer
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // written by the producer
    alignas(kCacheLine) T buffer_[N]{};
};

}  // namespace exch