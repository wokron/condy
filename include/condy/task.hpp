#pragma once

#include "condy/coro.hpp"
#include "condy/finish_handles.hpp"
#include <coroutine>
#include <exception>
#include <utility>

namespace condy {

template <typename PromiseType> class Task {
public:
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
        handle_.promise().set_auto_destroy(true);
        handle_ = nullptr;
    }

    auto operator co_await() &&;

    void cancel() {
        // TODO: support cancellation
        std::terminate();
    }

private:
    std::coroutine_handle<PromiseType> handle_;
};

template <typename PromiseType> auto Task<PromiseType>::operator co_await() && {
    struct TaskAwaiter {
        bool await_ready() noexcept {
            return task_handle_.promise().finished();
        }

        void await_suspend(std::coroutine_handle<> caller_handle) noexcept {
            task_handle_.promise().set_caller_handle(caller_handle);
        }

        void await_resume() noexcept { task_handle_.destroy(); }

        std::coroutine_handle<PromiseType> task_handle_;
    };

    return TaskAwaiter{std::exchange(handle_, nullptr)};
}

inline Task<Coro::promise_type> co_spawn(Coro coro) {
    auto handle = coro.release();
    auto *handle_ptr = new OpFinishHandle();
    handle_ptr->set_on_finish([handle, handle_ptr](int r) mutable {
        assert(r == 0);
        handle.resume();
        delete handle_ptr; // self delete
    });
    auto ring = Context::current().get_ring();
    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    assert(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, handle_ptr);

    return {handle};
}

} // namespace condy