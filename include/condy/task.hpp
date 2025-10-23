#pragma once

#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
#include <coroutine>
#include <exception>
#include <utility>

namespace condy {

template <typename T = void> class Task {
public:
    using PromiseType = typename Coro<T>::promise_type;

    Task(std::coroutine_handle<PromiseType> h) : handle_(h) {
        handle_.promise().set_auto_destroy(false);
    }
    Task(Task &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

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
        if (handle_.promise().finished()) {
            handle_.destroy();
        } else {
            handle_.promise().set_auto_destroy(true);
        }
        handle_ = nullptr;
    }

    auto operator co_await() &&;

private:
    std::coroutine_handle<PromiseType> handle_;
};

template <> inline auto Task<void>::operator co_await() && {
    struct TaskAwaiter {
        bool await_ready() noexcept {
            return task_handle_.promise().finished();
        }

        void await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            task_handle_.promise().set_caller_handle(caller_handle);
        }

        void await_resume() {
            auto exception = task_handle_.promise().exception();
            task_handle_.destroy();
            if (exception) {
                std::rethrow_exception(exception);
            }
        }

        std::coroutine_handle<typename Coro<void>::promise_type> task_handle_;
    };

    return TaskAwaiter{std::exchange(handle_, nullptr)};
}

template <typename T> inline auto Task<T>::operator co_await() && {
    struct TaskAwaiter {
        bool await_ready() noexcept {
            return task_handle_.promise().finished();
        }

        void await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            task_handle_.promise().set_caller_handle(caller_handle);
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
    };

    return TaskAwaiter{std::exchange(handle_, nullptr)};
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
    return {handle};
}

template <typename Executor> class TryPostAwaiter {
public:
    TryPostAwaiter(Executor &executor, OpFinishHandle *handle_to_executor)
        : executor_(executor), handle_to_executor_(handle_to_executor) {}
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

    void await_resume() noexcept {}

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
};

template <typename T, typename Executor>
inline auto co_spawn(Executor &executor, Coro<T> coro) {
    auto handle = coro.release();
    auto *handle_ptr = new OpFinishHandle();
    handle_ptr->set_on_finish([handle, handle_ptr](int r) mutable {
        assert(r == 0);
        auto *strategy = Context::current().get_strategy();
        if (strategy) { // Custom executor may not have context
            handle.promise().set_task_id(strategy->generate_task_id());
        }
        handle.resume();
        delete handle_ptr; // self delete
    });

    return TryPostAwaiter<Executor>(executor, handle_ptr);
}

} // namespace condy