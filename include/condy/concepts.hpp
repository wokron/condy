/**
 * @file concepts.hpp
 */

#pragma once

#include "condy/buffers.hpp"
#include "condy/invoker.hpp"
#include "condy/provided_buffers.hpp"
#include <coroutine>

namespace condy {

namespace detail {

struct FixedFd;

}

template <typename T>
concept HandleLike = requires(T handle, Invoker *invoker) {
    typename T::ReturnType;
    { handle.set_invoker(invoker) } -> std::same_as<void>;
    { handle.extract_result() } -> std::same_as<typename T::ReturnType>;
    { handle.cancel() } -> std::same_as<void>;
};

template <typename T>
concept OpFinishHandleLike =
    HandleLike<T> && requires(T handle, io_uring_cqe *cqe) {
        { handle.invoke() } -> std::same_as<void>;
        { handle.handle_cqe(cqe) } -> std::same_as<typename T::Action>;
    };

template <typename T>
concept AwaiterLike = requires(T awaiter) {
    typename T::HandleType;
    { awaiter.get_handle() } -> std::same_as<typename T::HandleType *>;
    { awaiter.init_finish_handle() } -> std::same_as<void>;
    { awaiter.register_operation(0) } -> std::same_as<void>;
    { awaiter.await_ready() } -> std::same_as<bool>;
    { awaiter.await_suspend(std::declval<std::coroutine_handle<>>()) };
    {
        awaiter.await_resume()
    } -> std::same_as<typename T::HandleType::ReturnType>;
};

template <typename T>
concept AwaiterRange =
    std::ranges::range<T> && AwaiterLike<std::ranges::range_value_t<T>>;

template <typename T>
concept BufferRingLike = requires(T br) {
    typename T::ReturnType;
    { br.bgid() } -> std::same_as<uint16_t>;
    { br.handle_finish(0, 0) } -> std::same_as<typename T::ReturnType>;
};

template <typename T>
concept BundledBufferRing =
    std::same_as<std::decay_t<T>, BundledProvidedBufferPool> ||
    std::same_as<std::decay_t<T>, BundledProvidedBufferQueue>;

template <typename T>
concept NotBundledBufferRing = BufferRingLike<T> && !BundledBufferRing<T>;

template <typename T>
concept BufferLike = std::derived_from<std::decay_t<T>, BufferBase>;

template <typename T>
concept FdLike = std::same_as<std::decay_t<T>, int> ||
                 std::same_as<std::decay_t<T>, detail::FixedFd>;

} // namespace condy