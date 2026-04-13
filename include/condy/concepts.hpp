/**
 * @file concepts.hpp
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <ranges>
#include <type_traits>

struct io_uring_cqe;
struct io_uring_sqe;

namespace condy {

class Ring;
class BufferBase;

namespace detail {

struct FixedFd;

} // namespace detail

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
concept SenderLike =
    requires(T sender) { typename std::decay_t<T>::ReturnType; };

template <typename T>
concept AwaiterLike = SenderLike<T>;

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