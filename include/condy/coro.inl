/**
 * @file coro.inl
 * @brief Coroutine implementation details.
 */

#pragma once

#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"
#include <coroutine>
#include <exception>
#include <mutex>
#include <new>
#include <optional>

namespace condy {

template <typename...> struct always_false {
    static constexpr bool value = false;
};

template <typename Allocator, typename... Args>
struct first_is_not_allocator : public std::true_type {};

template <typename Allocator, typename Arg, typename... Args>
struct first_is_not_allocator<Allocator, Arg, Args...> {
    static constexpr bool value =
        !std::is_same_v<std::remove_cvref_t<Arg>, Allocator>;
};

template <typename Promise, typename Allocator>
class BindAllocator : public Promise {
public:
#ifdef __clang__
    template <typename... Args>
        requires(first_is_not_allocator<Allocator, Args...>::value)
    static void *operator new(size_t, Args &&...) {
        // If user didn't provide a signature like (Allocator&, ...), clang will
        // fall back to ::new, we don't want that.
        // https://github.com/llvm/llvm-project/issues/54881
        static_assert(always_false<Args...>::value,
                      "Invalid arguments for allocator-bound coroutine");
    }
#endif

    template <typename... Args>
    static void *operator new(size_t size, Allocator &alloc, const Args &...) {
        size_t allocator_offset =
            (size + alignof(Allocator) - 1) & ~(alignof(Allocator) - 1);
        size_t total_size = allocator_offset + sizeof(Allocator);

        Pointer mem = alloc.allocate(total_size);
        try {
            new (mem + allocator_offset) Allocator(alloc);
        } catch (...) {
            alloc.deallocate(mem, total_size);
            throw;
        }
        return mem;
    }

    void operator delete(void *ptr, size_t size) noexcept {
        size_t allocator_offset =
            (size + alignof(Allocator) - 1) & ~(alignof(Allocator) - 1);
        size_t total_size = allocator_offset + sizeof(Allocator);
        Pointer mem = static_cast<Pointer>(ptr);
        Allocator &alloc = *std::launder(
            reinterpret_cast<Allocator *>(mem + allocator_offset));
        Allocator alloc_copy = std::move(alloc);
        alloc.~Allocator();
        alloc_copy.deallocate(mem, total_size);
    }

private:
    using Pointer = typename std::allocator_traits<Allocator>::pointer;
    using T = std::remove_pointer_t<Pointer>;
    static_assert(sizeof(T) == 1, "Allocator pointer must point to byte type");
};

template <typename Promise>
class BindAllocator<Promise, void> : public Promise {};

template <typename Coro>
class PromiseBase : public InvokerAdapter<PromiseBase<Coro>, WorkInvoker> {
public:
    using PromiseType = typename Coro::promise_type;

    ~PromiseBase() {
        if (exception_) [[unlikely]] {
            try {
                std::rethrow_exception(exception_);
            } catch (const std::exception &e) {
                panic_on(std::format(
                    "Unhandled exception in detached coroutine: {}", e.what()));
            } catch (...) {
                panic_on("Unhandled unknown exception in detached coroutine");
            }
        }
    }

    Coro get_return_object() noexcept {
        return Coro{std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this))};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<PromiseType> handle) noexcept {
            auto &self = handle.promise();

            State expected = self.state_.load(std::memory_order_acquire);
            State desired;
            do {
                if (expected == State::Idle) {
                    return self.caller_handle_;
                } else if (expected == State::RunningJoinable) {
                    desired = State::Zombie;
                } else if (expected == State::RunningDetached ||
                           expected == State::RunningJoining) {
                    desired = State::Finished;
                } else {
                    panic_on(std::format(
                        "Invalid coroutine state in final_suspend: {}",
                        static_cast<int>(expected)));
                }
            } while (!self.state_.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel,
                std::memory_order_acquire));

            State prev = expected;
            if (prev == State::RunningDetached) {
                handle.destroy();
                return std::noop_coroutine();
            } else if (prev == State::RunningJoining) {
                auto *callback = self.remote_callback_;
                (*callback)();
                return std::noop_coroutine();
            } else {
                assert(prev == State::RunningJoinable);
                return std::noop_coroutine();
            }
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() const noexcept { return {}; }

public:
    void mark_running() noexcept { state_ = State::RunningJoinable; }

    void request_detach() noexcept {
        State expected = state_.load(std::memory_order_acquire);
        State desired;
        do {
            if (expected == State::RunningJoinable) {
                desired = State::RunningDetached;
            } else if (expected == State::Zombie) {
                desired = State::Finished;
            } else {
                panic_on(
                    std::format("Invalid coroutine state in request_detach: {}",
                                static_cast<int>(expected)));
            }
        } while (!state_.compare_exchange_weak(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire));

        State prev = expected;
        if (prev == State::Zombie) {
            auto h = std::coroutine_handle<PromiseType>::from_promise(
                static_cast<PromiseType &>(*this));
            h.destroy();
        }
    }

    bool register_task_await(Invoker *remote_callback) noexcept {
        State expected = state_.load(std::memory_order_acquire);
        State desired;
        do {
            if (expected == State::RunningJoinable) {
                desired = State::RunningJoining;
                remote_callback_ = remote_callback;
            } else if (expected == State::Zombie) {
                desired = State::Finished;
            } else {
                panic_on(std::format(
                    "Invalid coroutine state in register_task_await: {}",
                    static_cast<int>(expected)));
            }
        } while (!state_.compare_exchange_weak(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire));

        State prev = expected;
        if (prev == State::Zombie) {
            return false; // ready to resume immediately
        } else {
            assert(prev == State::RunningJoinable);
            return true;
        }
    }

    void set_caller_handle(std::coroutine_handle<> handle) noexcept {
        caller_handle_ = handle;
    }

    std::exception_ptr exception() noexcept { return std::move(exception_); }

    void invoke() noexcept {
        auto h = std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this));
        h.resume();
    }

protected:
    enum class State : uint8_t {
        Idle,
        RunningJoinable,
        RunningDetached,
        RunningJoining,
        Zombie,
        Finished,
    };
    static_assert(std::atomic<State>::is_always_lock_free);

    std::atomic<State> state_ = State::Idle;
    union {
        std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
        Invoker *remote_callback_;
    };
    std::exception_ptr exception_;
};

template <typename Allocator>
class Promise<void, Allocator>
    : public BindAllocator<PromiseBase<Coro<void, Allocator>>, Allocator> {
public:
    void return_void() const noexcept {}
};

template <typename T, typename Allocator>
class Promise
    : public BindAllocator<PromiseBase<Coro<T, Allocator>>, Allocator> {
public:
    void return_value(T value) { value_ = std::move(value); }

    T value() { return std::move(value_.value()); }

private:
    std::optional<T> value_;
};

template <typename PromiseType> struct CoroAwaiterBase {
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<PromiseType>
    await_suspend(std::coroutine_handle<> caller_handle) noexcept {
        handle_.promise().set_caller_handle(caller_handle);
        return handle_;
    }

    std::coroutine_handle<PromiseType> handle_;
};

template <typename T, typename Allocator>
struct CoroAwaiter
    : public CoroAwaiterBase<typename Coro<T, Allocator>::promise_type> {
    using Base = CoroAwaiterBase<typename Coro<T, Allocator>::promise_type>;
    T await_resume() {
        auto exception = Base::handle_.promise().exception();
        if (exception) [[unlikely]] {
            Base::handle_.destroy();
            std::rethrow_exception(exception);
        }
        T value = Base::handle_.promise().value();
        Base::handle_.destroy();
        return value;
    }
};

template <typename Allocator>
struct CoroAwaiter<void, Allocator>
    : public CoroAwaiterBase<typename Coro<void, Allocator>::promise_type> {
    using Base = CoroAwaiterBase<typename Coro<void, Allocator>::promise_type>;
    void await_resume() {
        auto exception = Base::handle_.promise().exception();
        Base::handle_.destroy();
        if (exception) [[unlikely]] {
            std::rethrow_exception(exception);
        }
    }
};

template <typename T, typename Allocator>
inline auto Coro<T, Allocator>::operator co_await() noexcept {
    return CoroAwaiter<T, Allocator>{release()};
}

} // namespace condy