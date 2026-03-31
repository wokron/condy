/**
 * @file concepts.hpp
 */

#pragma once

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <ranges>
#include <type_traits>
#include <utility>

struct io_uring_cqe;
struct io_uring_sqe;

namespace condy {

class Ring;
class Invoker;
class BufferBase;

namespace detail {

struct FixedFd;
struct Action;

} // namespace detail

template <typename T>
concept HandleLike = requires(T handle, Invoker *invoker) {
    typename std::decay_t<T>::ReturnType;
    { handle.set_invoker(invoker) } -> std::same_as<void>;
    {
        handle.extract_result()
    } -> std::same_as<typename std::decay_t<T>::ReturnType>;
    { handle.cancel() } -> std::same_as<void>;
};

template <typename T>
concept OpFinishHandleLike =
    HandleLike<T> && requires(T handle, io_uring_cqe *cqe) {
        { handle.invoke() } -> std::same_as<void>;
        { handle.handle_cqe(cqe) } -> std::same_as<detail::Action>;
    };

template <typename T>
concept PrepFuncLike = requires(T prep_func, Ring *ring) {
    { prep_func(ring) } -> std::same_as<io_uring_sqe *>;
};

template <typename T>
concept CQEHandlerLike = requires(T handler, io_uring_cqe *cqe) {
    typename std::decay_t<T>::ReturnType;
    { handler.handle_cqe(cqe) } noexcept -> std::same_as<void>;
    {
        handler.extract_result()
    } noexcept -> std::same_as<typename std::decay_t<T>::ReturnType>;
};

template <typename T>
concept AwaiterLike = requires(T awaiter) {
    typename std::decay_t<T>::HandleType;
    {
        awaiter.get_handle()
    } -> std::same_as<typename std::decay_t<T>::HandleType *>;
    { awaiter.init_finish_handle() } -> std::same_as<void>;
    { awaiter.register_operation(0) } -> std::same_as<void>;
    { awaiter.await_ready() } noexcept -> std::same_as<bool>;
    { awaiter.await_suspend(std::declval<std::coroutine_handle<>>()) } noexcept;
    {
        awaiter.await_resume()
    } -> std::same_as<typename std::decay_t<T>::HandleType::ReturnType>;
};

template <typename T>
concept AwaiterRange =
    std::ranges::range<T> && AwaiterLike<std::ranges::range_value_t<T>>;

template <typename T>
concept BufferRingLike = requires(T br, io_uring_cqe *cqe) {
    typename std::decay_t<T>::ReturnType;
    { br.bgid() } -> std::same_as<uint16_t>;
    {
        br.handle_finish(cqe)
    } -> std::same_as<typename std::decay_t<T>::ReturnType>;
};

template <typename T>
concept BufferLike = std::derived_from<std::decay_t<T>, BufferBase>;

template <typename T>
concept FdLike = std::same_as<std::decay_t<T>, int> ||
                 std::same_as<std::decay_t<T>, detail::FixedFd>;

template <typename T, typename... Us>
concept AnySameAs = (std::same_as<T, Us> || ...);

} // namespace condy