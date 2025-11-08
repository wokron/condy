#pragma once

#include <coroutine>
#include <memory>
#include <utility>

namespace condy {

template <typename T, typename Allocator> class Promise;

template <typename T = void, typename Allocator = std::allocator<char>>
class [[nodiscard]] Coro {
public:
    using promise_type = Promise<T, Allocator>;

    Coro(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Coro(Coro &&other) noexcept : handle_(other.release()) {}

    Coro(const Coro &) = delete;
    Coro &operator=(const Coro &) = delete;
    Coro &operator=(Coro &&other) = delete;

    ~Coro() {
        if (handle_) {
            handle_.destroy();
        }
    }

public:
    auto operator co_await() &&;

    std::coroutine_handle<promise_type> release() noexcept {
        return std::exchange(handle_, nullptr);
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

} // namespace condy

#include "condy/coro.inl"