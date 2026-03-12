#pragma once

#include <coroutine>
#include <type_traits>
#include <utility>

namespace condy {

template <typename T>
struct is_nothrow_suspendible
    : std::bool_constant<noexcept(std::declval<T>().await_suspend(
          std::declval<std::coroutine_handle<>>()))> {};

template <typename T>
constexpr bool is_nothrow_suspendible_v = is_nothrow_suspendible<T>::value;

template <typename T>
struct is_nothrow_extract_result
    : std::bool_constant<noexcept(std::declval<T>().extract_result())> {};

template <typename T>
constexpr bool is_nothrow_extract_result_v =
    is_nothrow_extract_result<T>::value;

} // namespace condy