#pragma once

#include <coroutine>
#include <utility>

namespace condy {

template <typename T = void> class [[nodiscard]] Coro {
public:
    struct promise_type;

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