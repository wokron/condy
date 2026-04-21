/**
 * @file concepts.hpp
 */

#pragma once

#include <concepts>
#include <cstdint>
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

} // namespace condy