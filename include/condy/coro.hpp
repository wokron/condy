/**
 * @file coro.hpp
 * @brief Coroutine definitions.
 */

#pragma once

#include <coroutine>
#include <memory_resource>
#include <utility>

namespace condy {

template <typename T, typename Allocator> class Promise;

/**
 * @brief Coroutine type used to define a coroutine function.
 * @tparam T Return type of the coroutine.
 * @tparam Allocator Allocator type used for memory management, default is
 * `void`, which means use the default allocator.
 * @details User can define a coroutine function by specifying this type as the
 * return type. The coroutine function will return an instance of this type.
 * The coroutine can be awaited using the `co_await` operator, or can be
 * spawned as a task using @ref co_spawn().
 */
template <typename T = void, typename Allocator = void>
class [[nodiscard]] Coro {
public:
    using promise_type = Promise<T, Allocator>;

    Coro(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Coro(Coro &&other) noexcept : handle_(other.release()) {}

    Coro(const Coro &) = delete;
    Coro &operator=(const Coro &) = delete;
    Coro &operator=(Coro &&other) = delete;

    ~Coro() {
        if (handle_) {
            handle_.destroy();
        }
    }

public:
    /**
     * @brief Wait for the coroutine to complete and get the result.
     * @details User can use `co_await` operator to await the completion of the
     * coroutine and get the result. The behavior is similar to a normal
     * function call.
     * @return The result of the coroutine, with type `T`.
     */
    auto operator co_await() noexcept;

    std::coroutine_handle<promise_type> release() noexcept {
        return std::exchange(handle_, nullptr);
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

namespace pmr {

/**
 * @brief Coroutine type using polymorphic allocator.
 * @tparam T Return type of the coroutine.
 * @details This is a type alias for @ref condy::Coro that uses
 * `std::pmr::polymorphic_allocator<std::byte>` as the allocator type.
 */
template <typename T = void>
using Coro = condy::Coro<T, std::pmr::polymorphic_allocator<std::byte>>;

} // namespace pmr

} // namespace condy

#include "condy/coro.inl"