#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace condy {

struct Coro {
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

class Coro::promise_type {
public:
    Coro get_return_object() {
        return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    void return_void() noexcept {} // TODO: support return value

    void unhandled_exception() { exception_ = std::current_exception(); }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<promise_type> self) noexcept {
            auto caller_handler = self.promise().caller_handle_;
            if (self.promise().auto_destroy_) {
                self.destroy();
            }
            return caller_handler;
        }
        void await_resume() noexcept {}
    };
    auto final_suspend() noexcept {
        finished_ = true;
        return FinalAwaiter{};
    }

public:
    void set_caller_handle(std::coroutine_handle<> handle) noexcept {
        caller_handle_ = handle;
    }

    void set_auto_destroy(bool auto_destroy) noexcept {
        auto_destroy_ = auto_destroy;
    }

    bool finished() const noexcept { return finished_; }

    std::exception_ptr exception() const noexcept { return exception_; }

private:
    std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
    bool auto_destroy_ = true;
    bool finished_ = false;

    std::exception_ptr exception_;
};

inline auto Coro::operator co_await() && {
    struct CoroAwaiter {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<promise_type>
        await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            handle_.promise().set_auto_destroy(false);
            handle_.promise().set_caller_handle(caller_handle);
            return handle_;
        }

        void await_resume() {
            auto exception = handle_.promise().exception();
            handle_.destroy();
            if (exception) {
                std::rethrow_exception(exception);
            }
        }

        std::coroutine_handle<promise_type> handle_;
    };
    return CoroAwaiter{release()};
}

} // namespace condy
