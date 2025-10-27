#pragma once

#include "condy/condition_variable.hpp"
#include "condy/coro.hpp"
#include <cstddef>
#include <memory>

namespace condy {

template <typename T> class Channel {
public:
    Channel(size_t capacity)
        : capacity_(capacity), buffer_(std::make_unique<T[]>(capacity)) {
        assert(capacity > 0);
    }

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    Channel(Channel &&) = delete;
    Channel &operator=(Channel &&) = delete;

    Coro<void> send(T value) {
        auto guard = co_await mutex_.lock_guard();
        co_await cv_not_full_.wait([this] { return size_ < capacity_; });
        buffer_[tail_] = std::move(value);
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
        if (size_ == 1) {
            cv_not_empty_.notify_one();
        }
    }

    Coro<T> receive() {
        auto guard = co_await mutex_.lock_guard();
        co_await cv_not_empty_.wait([this] { return size_ > 0; });
        T value = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        if (size_ == capacity_ - 1) {
            cv_not_full_.notify_one();
        }
        co_return value;
    }

    Coro<size_t> size() {
        auto guard = co_await mutex_.lock_guard();
        co_return size_;
    }

private:
    Mutex mutex_;
    ConditionVariable cv_not_empty_{mutex_};
    ConditionVariable cv_not_full_{mutex_};
    size_t capacity_ = 0;
    std::unique_ptr<T[]> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
};

} // namespace condy