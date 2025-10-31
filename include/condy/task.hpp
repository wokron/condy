#pragma once

#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <coroutine>
#include <exception>
#include <future>
#include <utility>

namespace condy {

template <typename T = void> class [[nodiscard]] Task {
public:
    using PromiseType = typename Coro<T>::promise_type;

    Task(std::coroutine_handle<PromiseType> h, bool remote_task)
        : handle_(h), remote_task_(remote_task) {}
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

    T wait();

private:
    std::coroutine_handle<PromiseType> handle_;
    bool remote_task_;
};

template <> inline void Task<void>::wait() {
    if (!is_remote_task()) {
        throw std::logic_error(
            "Cannot wait on a non-remote task. Use co_await instead.");
    }
    std::promise<void> prom;
    auto fut = prom.get_future();
    struct TaskWaiter : public InvokerAdapter<TaskWaiter> {
        TaskWaiter(std::promise<void> &p) : prom_(p) {}

        void operator()() { prom_.set_value(); }

        std::promise<void> &prom_;
    };

    TaskWaiter waiter(prom);
    auto handle = std::exchange(handle_, nullptr);
    if (handle.promise().register_task_await(&waiter)) {
        // Still not finished, wait
        fut.get();
    }
    auto exception = handle.promise().exception();
    handle.destroy();
    if (exception) {
        std::rethrow_exception(exception);
    }
}

template <typename T> T Task<T>::wait() {
    if (!is_remote_task()) {
        throw std::logic_error(
            "Cannot wait on a non-remote task. Use co_await instead.");
    }
    std::promise<void> prom;
    auto fut = prom.get_future();
    struct TaskWaiter : public InvokerAdapter<TaskWaiter> {
        TaskWaiter(std::promise<void> &p) : prom_(p) {}

        void operator()() { prom_.set_value(); }

        std::promise<void> &prom_;
    };

    TaskWaiter waiter(prom);
    auto handle = std::exchange(handle_, nullptr);
    if (handle.promise().register_task_await(&waiter)) {
        // Still not finished, wait
        fut.get();
    }
    auto exception = handle.promise().exception();
    if (exception) {
        handle.destroy();
        std::rethrow_exception(exception);
    }
    T value = std::move(handle.promise()).value();
    handle.destroy();
    return std::move(value);
}

template <typename T>
struct TaskAwaiterBase : public InvokerAdapter<TaskAwaiterBase<T>> {
    TaskAwaiterBase(
        std::coroutine_handle<typename Coro<T>::promise_type> task_handle,
        IRuntime *runtime)
        : task_handle_(task_handle), runtime_(runtime) {}

    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool
    await_suspend(std::coroutine_handle<PromiseType> caller_handle) noexcept {
        Context::current().runtime()->pend_work();
        if (runtime_ == nullptr) {
            // No runtime provided, local task
            return task_handle_.promise().register_task_await(caller_handle);
        } else {
            // Remote task, need to post back to caller runtime
            caller_promise_ = &caller_handle.promise();
            return task_handle_.promise().register_task_await(this);
        }
    }

    void operator()() {
        assert(caller_promise_ != nullptr);
        runtime_->schedule(caller_promise_);
    }

    std::coroutine_handle<typename Coro<T>::promise_type> task_handle_;
    IRuntime *runtime_ = nullptr;
    Invoker *caller_promise_ = nullptr;
};

template <> inline auto Task<void>::operator co_await() && {
    struct TaskAwaiter : public TaskAwaiterBase<void> {
        using TaskAwaiterBase<void>::TaskAwaiterBase;

        void await_resume() {
            Context::current().runtime()->resume_work();
            auto exception = task_handle_.promise().exception();
            task_handle_.destroy();
            if (exception) {
                std::rethrow_exception(exception);
            }
        }
    };

    return TaskAwaiter(std::exchange(handle_, nullptr),
                       remote_task_ ? Context::current().runtime() : nullptr);
}

template <typename T> inline auto Task<T>::operator co_await() && {
    struct TaskAwaiter : public TaskAwaiterBase<T> {
        using Base = TaskAwaiterBase<T>;
        using Base::Base;

        T await_resume() {
            Context::current().runtime()->resume_work();
            auto exception = Base::task_handle_.promise().exception();
            if (exception) {
                Base::task_handle_.destroy();
                std::rethrow_exception(exception);
            }
            T value = std::move(Base::task_handle_.promise()).value();
            Base::task_handle_.destroy();
            return value;
        }
    };

    return TaskAwaiter(std::exchange(handle_, nullptr),
                       remote_task_ ? Context::current().runtime() : nullptr);
}

template <typename T> inline Task<T> co_spawn(Coro<T> coro) {
    auto handle = coro.release();
    auto &promise = handle.promise();
    promise.set_auto_destroy(false);

    Context::current().schedule_local(&promise);
    return {handle, false};
}

template <typename T, typename Runtime>
inline Task<T> co_spawn(Runtime &runtime, Coro<T> coro) {
    if (static_cast<IRuntime *>(&runtime) == Context::current().runtime()) {
        return co_spawn(std::move(coro));
    }

    auto handle = coro.release();
    auto &promise = handle.promise();
    promise.set_auto_destroy(false);
    promise.set_use_mutex(true);

    runtime.schedule(&promise);
    return {handle, true};
}

} // namespace condy