#pragma once

#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
#include "condy/retry.hpp"
#include <coroutine>
#include <exception>
#include <utility>

namespace condy {

template <typename T = void> class Task {
public:
    using PromiseType = typename Coro<T>::promise_type;

    Task(std::coroutine_handle<PromiseType> h, bool remote_task)
        : handle_(h), remote_task_(remote_task) {
        handle_.promise().set_auto_destroy(false);
        handle_.promise().set_use_mutex(remote_task_);
    }
    Task(Task &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          remote_task_(other.remote_task_) {}

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    Task &operator=(Task &&other) = delete;

    ~Task() {
        if (handle_) {
            std::terminate();
        }
    }

public:
    void detach() noexcept {
        handle_.promise().request_detach();
        handle_ = nullptr;
    }

    auto operator co_await() &&;

    bool is_remote_task() const noexcept { return remote_task_; }

private:
    std::coroutine_handle<PromiseType> handle_;
    bool remote_task_;
};

template <> inline auto Task<void>::operator co_await() && {
    struct TaskAwaiter {
        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            return task_handle_.promise().register_task_await(caller_handle,
                                                              loop_);
        }

        void await_resume() {
            auto exception = task_handle_.promise().exception();
            task_handle_.destroy();
            if (exception) {
                std::rethrow_exception(exception);
            }
        }

        std::coroutine_handle<typename Coro<void>::promise_type> task_handle_;
        IEventLoop *loop_ = nullptr;
    };

    return TaskAwaiter{std::exchange(handle_, nullptr),
                       remote_task_ ? Context::current().get_event_loop()
                                    : nullptr};
}

template <typename T> inline auto Task<T>::operator co_await() && {
    struct TaskAwaiter {
        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            return task_handle_.promise().register_task_await(caller_handle,
                                                              loop_);
        }

        T await_resume() {
            auto exception = task_handle_.promise().exception();
            if (exception) {
                task_handle_.destroy();
                std::rethrow_exception(exception);
            }
            T value = std::move(task_handle_.promise()).value();
            task_handle_.destroy();
            return value;
        }

        std::coroutine_handle<typename Coro<T>::promise_type> task_handle_;
        IEventLoop *loop_ = nullptr;
    };

    return TaskAwaiter{std::exchange(handle_, nullptr),
                       remote_task_ ? Context::current().get_event_loop()
                                    : nullptr};
}

template <typename T> inline Task<T> co_spawn(Coro<T> coro) {
    auto handle = coro.release();
    auto *strategy = Context::current().get_strategy();
    handle.promise().set_new_task(true);

    bool ok = Context::current().get_ready_queue()->try_enqueue(handle);
    if (!ok) { // Slow path
        auto *handle_ptr = new OpFinishHandle();
        handle_ptr->set_on_finish([handle, handle_ptr](int r) mutable {
            assert(r == 0);
            handle.resume();
            delete handle_ptr; // self delete
        });
        auto *ring = Context::current().get_ring();
        io_uring_sqe *sqe = strategy->get_sqe(ring);
        assert(sqe != nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, handle_ptr);
    }
    return {handle, false};
}

template <typename T, typename Executor>
inline Coro<Task<T>> co_spawn(Executor &executor, Coro<T> coro) {
    if (static_cast<IEventLoop *>(&executor) ==
        Context::current().get_event_loop()) {
        co_return co_spawn(std::move(coro));
    }
    auto handle = coro.release();
    handle.promise().set_new_task(true);

    co_await retry([&]() { return executor.try_post(handle); });
    co_return {handle, true};
}

} // namespace condy