#pragma once

#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"
#include <coroutine>
#include <exception>
#include <mutex>
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
        Allocator &alloc =
            *reinterpret_cast<Allocator *>(mem + allocator_offset);
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
        if (exception_) {
            panic_on("Unhandled exception in detached coroutine!!!");
        }
    }

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

            // 1. Detached task, destroy self
            if (self.auto_destroy_) {
                assert(self.caller_handle_ == std::noop_coroutine());
                lock.unlock();
                handle.destroy();
                return std::noop_coroutine();
            }

            // 2. Task awaited by another coroutine, invoke callback
            if (self.remote_callback_ != nullptr) {
                auto *callback = self.remote_callback_;
                assert(self.caller_handle_ == std::noop_coroutine());
                lock.unlock();
                (*callback)();
                return std::noop_coroutine();
            }

            // 3. Stacked coroutine, or task that has not been awaited yet
            self.finished_ = true;
            return self.caller_handle_;
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

    std::exception_ptr &exception() & noexcept { return exception_; }
    std::exception_ptr &&exception() && noexcept {
        return std::move(exception_);
    }

    void invoke() {
        auto h = std::coroutine_handle<PromiseType>::from_promise(
            static_cast<PromiseType &>(*this));
        h.resume();
    }

protected:
    std::mutex mutex_;
    std::coroutine_handle<> caller_handle_ = std::noop_coroutine();
    bool auto_destroy_ = true;
    bool finished_ = false;
    Invoker *remote_callback_ = nullptr;

    std::exception_ptr exception_;
};

template <typename Allocator>
class Promise<void, Allocator>
    : public BindAllocator<PromiseBase<Coro<void, Allocator>>, Allocator> {
public:
    void return_void() noexcept {}
};

template <typename T, typename Allocator>
class Promise
    : public BindAllocator<PromiseBase<Coro<T, Allocator>>, Allocator> {
public:
    void return_value(T value) { value_ = std::move(value); }

    T &value() & noexcept { return value_.value(); }
    T &&value() && noexcept { return std::move(value_).value(); }

private:
    std::optional<T> value_;
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

template <typename T, typename Allocator>
struct CoroAwaiter
    : public CoroAwaiterBase<typename Coro<T, Allocator>::promise_type> {
    using Base = CoroAwaiterBase<typename Coro<T, Allocator>::promise_type>;
    T await_resume() {
        auto exception = std::move(Base::handle_.promise()).exception();
        if (exception) [[unlikely]] {
            Base::handle_.destroy();
            std::rethrow_exception(exception);
        }
        T value = std::move(Base::handle_.promise()).value();
        Base::handle_.destroy();
        return value;
    }
};

template <typename Allocator>
struct CoroAwaiter<void, Allocator>
    : public CoroAwaiterBase<typename Coro<void, Allocator>::promise_type> {
    using Base = CoroAwaiterBase<typename Coro<void, Allocator>::promise_type>;
    void await_resume() {
        auto exception = std::move(Base::handle_.promise()).exception();
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