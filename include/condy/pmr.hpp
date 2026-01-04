/**
 * @file pmr.hpp
 * @brief Polymorphic memory resource (PMR) related types and utilities.
 */

#pragma once

#include "condy/coro.hpp"
#include "condy/task.hpp"
#include <memory_resource>

namespace condy {

/**
 * @brief Polymorphic memory resource (PMR) related types and utilities.
 */
namespace pmr {

/**
 * @brief Coroutine type using polymorphic allocator.
 * @tparam T Return type of the coroutine.
 * @details This is a type alias for @ref condy::Coro that uses
 * `std::pmr::polymorphic_allocator<std::byte>` as the allocator type.
 */
template <typename T = void>
using Coro = condy::Coro<T, std::pmr::polymorphic_allocator<std::byte>>;

/**
 * @brief Task type using polymorphic allocator.
 * @tparam T Return type of the coroutine.
 * @details This is a type alias for `condy::Task` that uses
 * `std::pmr::polymorphic_allocator<std::byte>` as the allocator type.
 */
template <typename T>
using Task = condy::Task<T, std::pmr::polymorphic_allocator<std::byte>>;

} // namespace pmr

} // namespace condy