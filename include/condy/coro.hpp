#pragma once

#include "condy/context.hpp"
#include "condy/strategies.hpp"
#include <coroutine>
#include <exception>
#include <utility>

namespace condy {

template <typename T = void> class Coro {
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

template <typename Coro, typename PromiseType> class PromiseBase {
public:
    std::suspend_always initial_suspend() noexcept { return {}; }

    void unhandled_exception() { exception_ = std::current_exception(); }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<PromiseType> self) noexcept {
            auto caller_handler = self.promise().caller_handle_;
            if (self.promise().auto_destroy_) {
                self.destroy();
            }
            return caller_handler;
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept {
        finished_ = true;
        if (task_id_ != -1) {
            Context::current().get_strategy()->recycle_task_id(task_id_);
        }
        return {};
    }

public:
    void request_detach() noexcept {
        if (!finished_) {
            auto_destroy_ = true;
        } else {
            // Destroy self immediately
            auto handle =
                std::coroutine_handle<PromiseType>::from_promise(*this);
            handle.destroy();
        }
    }

    bool register_task_await(std::coroutine_handle<> caller_handle) noexcept {
        if (finished_) {
            return false; // ready to resume immediately
        }
        caller_handle_ = caller_handle;
        return true;
    }

    void set_caller_handle(std::coroutine_handle<> handle) noexcept {
        caller_handle_ = handle;
    }

    void set_auto_destroy(bool auto_destroy) noexcept {
        auto_destroy_ = auto_destroy;
    }

    std::exception_ptr exception() const noexcept { return exception_; }

    void set_task_id(int id) noexcept { task_id_ = id; }

protected:
    std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
    bool auto_destroy_ = true;
    bool finished_ = false;

    int task_id_ = -1;

    std::exception_ptr exception_;
};

template <>
class Coro<void>::promise_type
    : public PromiseBase<Coro<void>, Coro<void>::promise_type> {
public:
    Coro get_return_object() {
        return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_void() noexcept {}
};

template <typename T>
class Coro<T>::promise_type
    : public PromiseBase<Coro<T>, Coro<T>::promise_type> {
public:
    Coro get_return_object() {
        return Coro{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    template <typename U>
    void
    return_value(U &&value) noexcept(std::is_nothrow_constructible_v<T, U &&>) {
        value_ = std::move(value);
    }

    T &value() & noexcept { return value_; }
    T &&value() && noexcept { return std::move(value_); }

private:
    T value_;
};

template <> inline auto Coro<void>::operator co_await() && {
    struct CoroAwaiter {
        bool await_ready() const noexcept { return false; }

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

template <typename T> inline auto Coro<T>::operator co_await() && {
    struct CoroAwaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<promise_type>
        await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            handle_.promise().set_auto_destroy(false);
            handle_.promise().set_caller_handle(caller_handle);
            return handle_;
        }

        T await_resume() {
            auto exception = handle_.promise().exception();
            if (exception) {
                handle_.destroy();
                std::rethrow_exception(exception);
            }
            T value = std::move(handle_.promise()).value();
            handle_.destroy();
            return value;
        }

        std::coroutine_handle<promise_type> handle_;
    };
    return CoroAwaiter{release()};
}

} // namespace condy
