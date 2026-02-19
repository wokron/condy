/**
 * @file task.hpp
 * @brief Interfaces for coroutine task management.
 * @details This file defines interfaces for running and managing concurrent
 * coroutine tasks in Condy.
 */

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

    TaskBase() : TaskBase(nullptr) {}
    TaskBase(std::coroutine_handle<PromiseType> h) : handle_(h) {}
    TaskBase(TaskBase &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    TaskBase &operator=(TaskBase &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                panic_on("Task destroyed without being awaited");
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    TaskBase(const TaskBase &) = delete;
    TaskBase &operator=(const TaskBase &) = delete;

    ~TaskBase() {
        if (handle_) {
            panic_on("Task destroyed without being awaited");
        }
    }

public:
    /**
     * @brief Detach the task to run independently.
     * @details This function detaches the task, allowing it to run
     * independently in the associated runtime. The caller will not be able to
     * await the task or retrieve its result after detachment.
     * @warning Unhandled exceptions in a detached task will cause a panic.
     */
    void detach() noexcept {
        handle_.promise().request_detach();
        handle_ = nullptr;
    }

    /**
     * @brief Check if the task is still awaitable. Similar to
     * `std::thread::joinable()`.
     */
    bool awaitable() const noexcept { return handle_ != nullptr; }

    /**
     * @brief Await the task asynchronously.
     * @return T The result of the coroutine.
     * @throw std::invalid_argument If the task is not awaitable.
     * @throws Any exception thrown inside the coroutine.
     * @details This function allows the caller to await the completion of the
     * coroutine associated with the task. It suspends the caller coroutine
     * until the task completes, and then retrieves the result of the coroutine.
     * If the coroutine throws an exception, it will be rethrown here.
     */
    auto operator co_await() noexcept;

protected:
    static void wait_inner_(std::coroutine_handle<PromiseType> handle);

protected:
    std::coroutine_handle<PromiseType> handle_;
};

template <typename T, typename Allocator>
void TaskBase<T, Allocator>::wait_inner_(
    std::coroutine_handle<PromiseType> handle) {
    if (detail::Context::current().runtime() != nullptr) [[unlikely]] {
        throw std::logic_error("Sync wait inside runtime");
    }
    if (handle == nullptr) [[unlikely]] {
        throw std::invalid_argument("Task not awaitable");
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

/**
 * @brief Coroutine task that runs concurrently in the runtime.
 * @tparam T Return type of the coroutine.
 * @tparam Allocator Allocator type used for memory management, default is
 * `void`, which means use the default allocator.
 * @details Task is a handle to a coroutine task that can be awaited or
 * detached. The coroutine will be scheduled and executed in the associated
 * runtime. User can await the task to get the result of the coroutine, or
 * detach the task to let it run independently.
 * @warning Destroying a Task without awaiting or detaching it will cause a
 * panic.
 * @warning Unhandled exceptions in a detached task will also cause a panic.
 */
template <typename T = void, typename Allocator = void>
class [[nodiscard]] Task : public TaskBase<T, Allocator> {
public:
    using Base = TaskBase<T, Allocator>;
    using Base::Base;

    /**
     * @brief Wait synchronously for the task to complete and get the result.
     * @return T The result of the coroutine.
     * @throws std::invalid_argument If the task is not awaitable.
     * @throws Any exception thrown inside the coroutine.
     * @details This function blocks the current thread until the coroutine
     * associated with the task completes. It then retrieves the result of the
     * coroutine. If the coroutine throws an exception, it will be rethrown
     * here.
     */
    T wait() {
        auto handle = std::exchange(Base::handle_, nullptr);
        Base::wait_inner_(handle);
        auto exception = std::move(handle.promise()).exception();
        if (exception) [[unlikely]] {
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

    /**
     * @brief Wait synchronously for the task to complete.
     * @throws std::invalid_argument If the task is not awaitable.
     * @throws Any exception thrown inside the coroutine.
     * @details This function blocks the current thread until the coroutine
     * associated with the task completes. If the coroutine throws an exception,
     * it will be rethrown here.
     */
    void wait() {
        auto handle = std::exchange(Base::handle_, nullptr);
        Base::wait_inner_(handle);
        auto exception = std::move(handle.promise()).exception();
        handle.destroy();
        if (exception) [[unlikely]] {
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

    bool await_ready() const {
        if (task_handle_ == nullptr) {
            throw std::invalid_argument("Task not awaitable");
        }
        return false;
    }

    template <typename PromiseType>
    bool
    await_suspend(std::coroutine_handle<PromiseType> caller_handle) noexcept {
        detail::Context::current().runtime()->pend_work();
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
        detail::Context::current().runtime()->resume_work();
        auto exception = std::move(Base::task_handle_.promise()).exception();
        if (exception) [[unlikely]] {
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
        detail::Context::current().runtime()->resume_work();
        auto exception = std::move(Base::task_handle_.promise()).exception();
        Base::task_handle_.destroy();
        if (exception) [[unlikely]] {
            std::rethrow_exception(exception);
        }
    }
};

template <typename T, typename Allocator>
inline auto TaskBase<T, Allocator>::operator co_await() noexcept {
    return TaskAwaiter<T, Allocator>(std::exchange(handle_, nullptr),
                                     detail::Context::current().runtime());
}

/**
 * @brief Spawn a coroutine as a task in the given runtime.
 * @tparam T Return type of the coroutine.
 * @tparam Allocator Allocator type used for memory management.
 * @param runtime The runtime to spawn the coroutine in.
 * @param coro The coroutine to be spawned.
 * @return Task<T, Allocator> The spawned task.
 * @details This function schedules the given coroutine to run as a task in the
 * specified runtime. The coroutine will be executed concurrently in the
 * runtime.
 */
template <typename T, typename Allocator>
inline Task<T, Allocator> co_spawn(Runtime &runtime, Coro<T, Allocator> coro) {
    auto handle = coro.release();
    auto &promise = handle.promise();
    promise.set_auto_destroy(false);

    runtime.schedule(&promise);
    return {handle};
}

/**
 * @brief Spawn a coroutine as a task in the current runtime.
 * @tparam T Return type of the coroutine.
 * @tparam Allocator Allocator type used for memory management.
 * @param coro The coroutine to be spawned.
 * @return Task<T, Allocator> The spawned task.
 * @throws std::logic_error If there is no current runtime.
 */
template <typename T, typename Allocator>
inline Task<T, Allocator> co_spawn(Coro<T, Allocator> coro) {
    auto *runtime = detail::Context::current().runtime();
    if (runtime == nullptr) [[unlikely]] {
        throw std::logic_error("No runtime to spawn coroutine task");
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

/**
 * @brief Switch current coroutine task to the given runtime.
 * @param runtime The runtime to switch to.
 * @return detail::SwitchAwaiter Awaiter object for the switch operation.
 * @details This function suspends the current coroutine and reschedules it to
 * run in the specified runtime. The caller coroutine will be resumed in the
 * target runtime.
 */
inline detail::SwitchAwaiter co_switch(Runtime &runtime) { return {&runtime}; }

} // namespace condy