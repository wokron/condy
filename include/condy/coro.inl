#pragma once

#include "condy/coro.hpp"
#include "condy/event_loop.hpp"
#include "condy/finish_handles.hpp"
#include "condy/spin_lock.hpp"
#include "condy/uninitialized.hpp"
#include "condy/utils.hpp"
#include <coroutine>

namespace condy {

template <typename Coro> class PromiseBase {
public:
    using PromiseType = typename Coro::promise_type;

    Coro get_return_object() {
        return Coro{std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this))};
    }

    struct InitialAwaiter {
        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
            self_ = h;
        }

        void await_resume() noexcept {
            auto &promise = self_.promise();
            if (promise.new_task_) {
                promise.task_id_ =
                    Context::current().get_strategy()->generate_task_id();
            }
        }

        std::coroutine_handle<PromiseType> self_;
    };

    InitialAwaiter initial_suspend() noexcept { return {}; }

    void unhandled_exception() { exception_ = std::current_exception(); }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<PromiseType> handle) noexcept {
            auto &self = handle.promise();
            std::lock_guard lock(self.mutex_);

            auto caller_handle = self.caller_handle_;

            // 1. Common coro or detached task, destroy self
            if (self.auto_destroy_) {
                handle.destroy();
                return caller_handle;
            }

            // 2. Task awaited by another coroutine in different event loops,
            // need to post back to caller loop
            if (self.caller_loop_ != nullptr) {
                assert(caller_handle != std::noop_coroutine());
                OpFinishHandle *handle = new OpFinishHandle();
                handle->set_on_finish([caller_handle, handle](int r) mutable {
                    assert(r == 0);
                    caller_handle.resume();
                    delete handle; // self delete
                });

                if (!self.caller_loop_->try_post(handle)) {
                    auto *retry_handle = new RetryFinishHandle<>();
                    auto on_retry = [retry_handle, handle,
                                     caller_loop = self.caller_loop_] {
                        bool ok = caller_loop->try_post(handle);
                        if (ok) {
                            delete retry_handle; // self delete
                        }
                        return ok;
                    };
                    retry_handle->set_on_retry(std::move(on_retry),
                                               std::noop_coroutine());
                    retry_handle->prep_retry();
                }

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

    FinalAwaiter final_suspend() noexcept {
        if (new_task_) {
            assert(task_id_ != -1);
            Context::current().get_strategy()->recycle_task_id(task_id_);
        }
        return {};
    }

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

    bool register_task_await(std::coroutine_handle<> caller_handle,
                             IEventLoop *caller_loop) noexcept {
        std::lock_guard lock(mutex_);
        if (finished_) {
            return false; // ready to resume immediately
        }
        caller_handle_ = caller_handle;
        caller_loop_ = caller_loop;
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

    void set_new_task(bool is_new) noexcept { new_task_ = is_new; }

protected:
    MaybeMutex<SpinLock> mutex_;
    std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
    bool auto_destroy_ = true;
    bool finished_ = false;
    IEventLoop *caller_loop_ = nullptr;

    int task_id_ = -1;
    bool new_task_ = false;

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