#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace chronos {

// Bounded lock-free single-producer / single-consumer queue.
// Capacity must be a power of two so indexing can use a mask.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
public:
    bool try_push(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity) return false;
        buffer_[head & mask_] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t approximate_size() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;
    alignas(64) std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace chronos
