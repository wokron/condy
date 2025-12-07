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

template <typename T = void, typename Allocator = void> class TaskBase {
public:
    using PromiseType = typename Coro<T, Allocator>::promise_type;

    TaskBase(std::coroutine_handle<PromiseType> h) : handle_(h) {}
    TaskBase(TaskBase &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    TaskBase(const TaskBase &) = delete;
    TaskBase &operator=(const TaskBase &) = delete;
    TaskBase &operator=(TaskBase &&other) = delete;

    ~TaskBase() {
        if (handle_) {
            panic_on("Task destroyed without being awaited");
        }
    }

public:
    void detach() noexcept {
        handle_.promise().request_detach();
        handle_ = nullptr;
    }

    auto operator co_await() &&;

protected:
    static void wait_inner_(std::coroutine_handle<PromiseType> handle);

protected:
    std::coroutine_handle<PromiseType> handle_;
};

template <typename T, typename Allocator>
void TaskBase<T, Allocator>::wait_inner_(
    std::coroutine_handle<PromiseType> handle) {
    if (Context::current().runtime() != nullptr) {
        throw std::logic_error("Potential deadlock: cannot wait on a task from "
                               "within a runtime context.");
    }
    std::promise<void> prom;
    auto fut = prom.get_future();
    struct TaskWaiter : public InvokerAdapter<TaskWaiter> {
        TaskWaiter(std::promise<void> &p) : prom_(p) {}

        void invoke() { prom_.set_value(); }

        std::promise<void> &prom_;
    };

    TaskWaiter waiter(prom);
    if (handle.promise().register_task_await(&waiter)) {
        // Still not finished, wait
        fut.get();
    }
}

template <typename T = void, typename Allocator = void>
class [[nodiscard]] Task : public TaskBase<T, Allocator> {
public:
    using Base = TaskBase<T, Allocator>;
    using Base::Base;

    T wait() {
        auto handle = std::exchange(Base::handle_, nullptr);
        Base::wait_inner_(handle);
        auto exception = std::move(handle.promise()).exception();
        if (exception) {
            handle.destroy();
            std::rethrow_exception(exception);
        }
        T value = std::move(handle.promise()).value();
        handle.destroy();
        return std::move(value);
    }
};

template <typename Allocator>
class [[nodiscard]] Task<void, Allocator> : public TaskBase<void, Allocator> {
public:
    using Base = TaskBase<void, Allocator>;
    using Base::Base;

    void wait() {
        auto handle = std::exchange(Base::handle_, nullptr);
        Base::wait_inner_(handle);
        auto exception = std::move(handle.promise()).exception();
        handle.destroy();
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

template <typename T, typename Allocator>
struct TaskAwaiterBase : public InvokerAdapter<TaskAwaiterBase<T, Allocator>> {
    TaskAwaiterBase(
        std::coroutine_handle<typename Coro<T, Allocator>::promise_type>
            task_handle,
        Runtime *runtime)
        : task_handle_(task_handle), runtime_(runtime) {}

    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool
    await_suspend(std::coroutine_handle<PromiseType> caller_handle) noexcept {
        Context::current().runtime()->pend_work();
        assert(runtime_ != nullptr);
        caller_promise_ = &caller_handle.promise();
        return task_handle_.promise().register_task_await(this);
    }

    void invoke() {
        assert(caller_promise_ != nullptr);
        runtime_->schedule(caller_promise_);
    }

    std::coroutine_handle<typename Coro<T, Allocator>::promise_type>
        task_handle_;
    Runtime *runtime_ = nullptr;
    WorkInvoker *caller_promise_ = nullptr;
};

template <typename T, typename Allocator>
struct TaskAwaiter : public TaskAwaiterBase<T, Allocator> {
    using Base = TaskAwaiterBase<T, Allocator>;
    using Base::Base;

    T await_resume() {
        Context::current().runtime()->resume_work();
        auto exception = std::move(Base::task_handle_.promise()).exception();
        if (exception) {
            Base::task_handle_.destroy();
            std::rethrow_exception(exception);
        }
        T value = std::move(Base::task_handle_.promise()).value();
        Base::task_handle_.destroy();
        return value;
    }
};

template <typename Allocator>
struct TaskAwaiter<void, Allocator> : public TaskAwaiterBase<void, Allocator> {
    using Base = TaskAwaiterBase<void, Allocator>;
    using Base::Base;

    void await_resume() {
        Context::current().runtime()->resume_work();
        auto exception = std::move(Base::task_handle_.promise()).exception();
        Base::task_handle_.destroy();
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

template <typename T, typename Allocator>
inline auto TaskBase<T, Allocator>::operator co_await() && {
    return TaskAwaiter<T, Allocator>(std::exchange(handle_, nullptr),
                                     Context::current().runtime());
}

template <typename T, typename Allocator>
inline Task<T, Allocator> co_spawn(Runtime &runtime, Coro<T, Allocator> coro) {
    auto handle = coro.release();
    auto &promise = handle.promise();
    promise.set_auto_destroy(false);

    runtime.schedule(&promise);
    return {handle};
}

template <typename T, typename Allocator>
inline Task<T, Allocator> co_spawn(Coro<T, Allocator> coro) {
    auto *runtime = Context::current().runtime();
    if (runtime == nullptr) {
        throw std::logic_error(
            "No runtime in current context to spawn the task");
    }
    return co_spawn(*runtime, std::move(coro));
}

namespace detail {

struct [[nodiscard]] SwitchAwaiter {
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> handle) noexcept {
        runtime_->schedule(&handle.promise());
    }

    void await_resume() const noexcept {}

    Runtime *runtime_;
};

} // namespace detail

inline detail::SwitchAwaiter co_switch(Runtime &runtime) { return {&runtime}; }

namespace pmr {

template <typename T>
using Task = condy::Task<T, std::pmr::polymorphic_allocator<std::byte>>;

}

} // namespace condy