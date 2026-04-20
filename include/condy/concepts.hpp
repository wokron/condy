/**
 * @file concepts.hpp
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <stop_token>
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
    { handler(cqe) } noexcept;
};

template <typename T>
concept BufferRingLike = requires(T br, io_uring_cqe *cqe) {
    { br.bgid() } -> std::same_as<uint16_t>;
    { br.handle_finish(cqe) };
};

template <typename T>
concept BufferLike = std::derived_from<std::decay_t<T>, BufferBase>;

template <typename T>
concept FdLike = std::same_as<std::decay_t<T>, int> ||
                 std::same_as<std::decay_t<T>, detail::FixedFd>;

template <typename T, typename... Us>
concept AnySameAs = (std::same_as<T, Us> || ...);

namespace detail {

template <typename T, typename Callback>
concept HasStopCallback =
    requires { typename T::template callback_type<Callback>; };

template <typename T, typename Callback> struct stop_callback_of;
template <typename T, typename Callback>
    requires HasStopCallback<T, Callback>
struct stop_callback_of<T, Callback> {
    using type = typename T::template callback_type<Callback>;
};
template <typename T, typename Callback>
    requires(!HasStopCallback<T, Callback> &&
             std::is_same_v<T, std::stop_token>)
struct stop_callback_of<T, Callback> {
    using type = std::stop_callback<Callback>;
};

} // namespace detail

template <typename T> struct stop_callback_of {
    template <typename Callback>
    using type = typename detail::stop_callback_of<T, Callback>::type;
};

} // namespace condy