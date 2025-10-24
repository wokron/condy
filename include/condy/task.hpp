#pragma once

#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
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
        EventLoop *loop_ = nullptr;
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
        EventLoop *loop_ = nullptr;
    };

    return TaskAwaiter{std::exchange(handle_, nullptr),
                       remote_task_ ? Context::current().get_event_loop()
                                    : nullptr};
}

template <typename T> inline Task<T> co_spawn(Coro<T> coro) {
    auto handle = coro.release();
    auto *strategy = Context::current().get_strategy();
    int task_id = strategy->generate_task_id();
    handle.promise().set_task_id(task_id);
    auto *handle_ptr = new OpFinishHandle();
    handle_ptr->set_on_finish([handle, handle_ptr](int r) mutable {
        assert(r == 0);
        handle.resume();
        delete handle_ptr; // self delete
    });

    bool ok = Context::current().get_ready_queue()->try_enqueue(handle_ptr);
    if (!ok) { // Slow path
        auto *ring = Context::current().get_ring();
        io_uring_sqe *sqe = strategy->get_sqe(ring);
        assert(sqe != nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, handle_ptr);
    }
    return {handle, false};
}

template <typename Executor, typename T> class TryPostAwaiter {
public:
    TryPostAwaiter(Executor &executor, OpFinishHandle *handle_to_executor,
                   Task<T> task)
        : executor_(executor), handle_to_executor_(handle_to_executor),
          task_(std::move(task)) {}
    TryPostAwaiter(TryPostAwaiter &&) = default;

    TryPostAwaiter(const TryPostAwaiter &) = delete;
    TryPostAwaiter &operator=(const TryPostAwaiter &) = delete;
    TryPostAwaiter &operator=(TryPostAwaiter &&) = delete;

public:
    bool await_ready() { return executor_.try_post(handle_to_executor_); }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        finish_handle_.set_on_finish(
            [h, this](typename OpFinishHandle::ReturnType r) {
                bool ok = executor_.try_post(handle_to_executor_);
                if (!ok) {
                    retry_later_();
                } else {
                    h.resume();
                }
            });
        retry_later_();
    }

    Task<T> await_resume() noexcept { return std::move(task_); }

private:
    void retry_later_() {
        auto &context = Context::current();
        auto ring = context.get_ring();
        auto *sqe = context.get_strategy()->get_sqe(ring);
        assert(sqe != nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, &finish_handle_);
    }

private:
    Executor &executor_;
    OpFinishHandle *handle_to_executor_;
    OpFinishHandle finish_handle_;
    Task<T> task_;
};

template <typename T, typename Executor>
inline auto co_spawn(Executor &executor, Coro<T> coro) {
    auto handle = coro.release();
    auto *handle_ptr = new OpFinishHandle();
    handle_ptr->set_on_finish([handle, handle_ptr](int r) mutable {
        assert(r == 0);
        handle.promise().set_task_id(
            Context::current().get_strategy()->generate_task_id());
        handle.resume();
        delete handle_ptr; // self delete
    });

    return TryPostAwaiter<Executor, T>(executor, handle_ptr,
                                       Task<T>{handle, true});
}

} // namespace condy