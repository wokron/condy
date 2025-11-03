#pragma once

#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/uninitialized.hpp"
#include "condy/utils.hpp"
#include <coroutine>
#include <mutex>

namespace condy {

template <typename Coro>
class PromiseBase : public InvokerAdapter<PromiseBase<Coro>, WorkInvoker> {
public:
    using PromiseType = typename Coro::promise_type;

    Coro get_return_object() {
        return Coro{std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this))};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    void unhandled_exception() { exception_ = std::current_exception(); }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<PromiseType> handle) noexcept {
            auto &self = handle.promise();
            std::unique_lock lock(self.mutex_);

            auto caller_handle = self.caller_handle_;

            // 1. Common coro or detached task, destroy self
            if (self.auto_destroy_) {
                lock.unlock();
                handle.destroy();
                return caller_handle;
            }

            // 2. Task awaited by another coroutine in different event loops,
            // need to post back to caller loop
            if (self.remote_callback_ != nullptr) {
                auto *callback = self.remote_callback_;
                lock.unlock();
                (*callback)();
                return std::noop_coroutine();
            }

            // 3. Task awaited by another coroutine in the same event loop,
            // or task that has not been awaited yet (noop_coroutine), just
            // resume caller
            self.finished_ = true;
            return caller_handle;
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

public:
    void request_detach() noexcept {
        std::lock_guard lock(mutex_);
        if (!finished_) {
            auto_destroy_ = true;
        } else {
            // Destroy self immediately
            auto handle = std::coroutine_handle<PromiseType>::from_promise(
                static_cast<PromiseType &>(*this));
            handle.destroy();
        }
    }

    bool register_task_await(std::coroutine_handle<> caller_handle) noexcept {
        std::lock_guard lock(mutex_);
        if (finished_) {
            return false; // ready to resume immediately
        }
        caller_handle_ = caller_handle;
        return true;
    }

    bool register_task_await(Invoker *remote_callback) noexcept {
        std::lock_guard lock(mutex_);
        if (finished_) {
            return false; // ready to resume immediately
        }
        remote_callback_ = remote_callback;
        return true;
    }

    void set_caller_handle(std::coroutine_handle<> handle) noexcept {
        caller_handle_ = handle;
    }

    void set_auto_destroy(bool auto_destroy) noexcept {
        auto_destroy_ = auto_destroy;
    }

    void set_use_mutex(bool use_mutex) noexcept {
        mutex_.set_use_mutex(use_mutex);
    }

    std::exception_ptr exception() const noexcept { return exception_; }

    void operator()() {
        auto h = std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this));
        h.resume();
    }

protected:
    MaybeMutex<std::mutex> mutex_;
    std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
    bool auto_destroy_ = true;
    bool finished_ = false;
    Invoker *remote_callback_ = nullptr;

    std::exception_ptr exception_;
};

template <> class Coro<void>::promise_type : public PromiseBase<Coro<void>> {
public:
    void return_void() noexcept {}
};

template <typename T>
class Coro<T>::promise_type : public PromiseBase<Coro<T>> {
public:
    void return_value(T value) { value_.emplace(std::move(value)); }

    T &value() & noexcept { return value_.get(); }
    T &&value() && noexcept { return std::move(value_.get()); }

private:
    Uninitialized<T> value_;
};

template <typename PromiseType> struct CoroAwaiterBase {
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<PromiseType>
    await_suspend(std::coroutine_handle<> caller_handle) noexcept {
        handle_.promise().set_auto_destroy(false);
        handle_.promise().set_caller_handle(caller_handle);
        return handle_;
    }

    std::coroutine_handle<PromiseType> handle_;
};

template <> inline auto Coro<void>::operator co_await() && {
    struct CoroAwaiter : public CoroAwaiterBase<promise_type> {
        void await_resume() {
            auto exception = handle_.promise().exception();
            handle_.destroy();
            if (exception) {
                std::rethrow_exception(exception);
            }
        }
    };
    return CoroAwaiter{release()};
}

template <typename T> inline auto Coro<T>::operator co_await() && {
    struct CoroAwaiter : public CoroAwaiterBase<promise_type> {
        using Base = CoroAwaiterBase<promise_type>;
        T await_resume() {
            auto exception = Base::handle_.promise().exception();
            if (exception) {
                Base::handle_.destroy();
                std::rethrow_exception(exception);
            }
            T value = std::move(Base::handle_.promise()).value();
            Base::handle_.destroy();
            return value;
        }
    };
    return CoroAwaiter{release()};
}

} // namespace condy