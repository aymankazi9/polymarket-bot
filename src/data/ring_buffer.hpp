#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>

// SPSC (single-producer / single-consumer) lock-free ring buffer.
//
// N must be a power of two.  head_ is advanced by the producer only;
// tail_ is advanced by the consumer only.  The counters are allowed to
// wrap freely — unsigned overflow is well-defined — so the usable capacity
// is N - 1 slots (head == tail means empty; head - tail == N means full).
//
// Memory ordering:
//   push: reads tail_ with acquire (so we see consumer's progress before
//         deciding full/empty), writes buf_ before releasing head_.
//   pop:  reads head_ with acquire (so we see producer's write to buf_
//         before we read it), releases tail_ after copying the element.
//
// T must be trivially copyable.

namespace data {

template<typename T, size_t N>
class SPSCRingBuffer {
    static_assert((N & (N - 1)) == 0, "SPSCRingBuffer: N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    static constexpr size_t MASK = N - 1;

    T buf_[N];

    // Separate cache lines to avoid false sharing between producer and consumer
    alignas(64) std::atomic<size_t> head_{0};  // next write slot (producer)
    alignas(64) std::atomic<size_t> tail_{0};  // next read  slot (consumer)

public:
    // Producer: enqueue one element.
    // Returns false (and drops) if the buffer is full.
    bool push(const T& v) noexcept {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h - t == N) return false;           // full
        buf_[h & MASK] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer: dequeue one element into `out`.
    // Returns false if the buffer is empty.
    bool pop(T& out) noexcept {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;               // empty
        out = buf_[t & MASK];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Approximate occupancy — not precise across threads
    size_t size_approx() const noexcept {
        return head_.load(std::memory_order_relaxed) -
               tail_.load(std::memory_order_relaxed);
    }

    bool empty() const noexcept { return size_approx() == 0; }
    bool full()  const noexcept { return size_approx() == N; }

    static constexpr size_t capacity() noexcept { return N; }
};

} // namespace data
