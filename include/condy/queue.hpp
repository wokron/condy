#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>

namespace condy {

template <typename T> T round_up_pow2(T n) {
    if (n == 0)
        return 1;
    n--;
    for (size_t i = 1; i < sizeof(T) * 8; i *= 2) {
        n |= n >> i;
    }
    n++;
    return n;
}

template <typename T, template <typename> class Atomic = std::atomic>
class RingQueue {
public:
    RingQueue(size_t capacity)
        : data_(std::make_unique<T[]>(round_up_pow2(capacity))),
          mask_(round_up_pow2(capacity) - 1) {}

public:
    std::optional<T> try_dequeue() {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return std::nullopt;
        }
        T item = std::move(data_[head & mask_]);
        head_.store(head + 1, std::memory_order_relaxed);
        return item;
    }

    template <typename U> bool try_enqueue(U &&item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (((tail + 1) & mask_) == (head & mask_)) {
            return false;
        }
        data_[tail & mask_] = std::forward<U>(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size_unreliable() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        assert(tail >= head);
        return tail - head;
    }

private:
    std::unique_ptr<T[]> data_;
    size_t mask_;
    Atomic<size_t> head_{0}, tail_{0};
};

template <typename T> class FakeAtomic {
public:
    FakeAtomic() = default;
    FakeAtomic(T value) : value_(value) {}
    FakeAtomic(const FakeAtomic &) = delete;
    FakeAtomic &operator=(const FakeAtomic &) = delete;
    T load(std::memory_order) const { return value_; }
    void store(T value, std::memory_order) { value_ = value; }

private:
    T value_;
};

template <typename T> using SingleThreadRingQueue = RingQueue<T, FakeAtomic>;

template <typename T, template <typename> class Atomic = std::atomic>
class MultiWriterRingQueue : public RingQueue<T, Atomic> {
public:
    using Base = RingQueue<T, Atomic>;
    using Base::Base;

    template <typename U> bool try_enqueue(U &&item) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return Base::try_enqueue(std::forward<U>(item));
    }

private:
    std::mutex write_mutex_;
};

} // namespace condy