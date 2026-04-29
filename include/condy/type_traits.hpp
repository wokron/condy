/**
 * @file type_traits.hpp
 */

#pragma once

#include <stop_token>
#include <type_traits>
#include <utility>

namespace condy {

namespace detail {

template <typename T, typename Callback>
concept HasStopCallback =
    requires { typename T::template callback_type<Callback>; };

template <typename T, typename Callback> struct stop_callback_traits;
template <typename T, typename Callback>
    requires HasStopCallback<T, Callback>
struct stop_callback_traits<T, Callback> {
    using type = typename T::template callback_type<Callback>;
};
template <typename T, typename Callback>
    requires(!HasStopCallback<T, Callback> &&
             std::is_same_v<T, std::stop_token>)
struct stop_callback_traits<T, Callback> {
    using type = std::stop_callback<Callback>;
};

} // namespace detail

template <typename T, typename Callback>
using stop_callback_t =
    typename detail::stop_callback_traits<T, Callback>::type;

template <typename Sender, typename Receiver>
using operation_state_t = decltype(std::declval<Sender &&>().connect_impl(
    std::declval<Receiver &&>()));

template <typename Receiver>
using stop_token_t =
    std::remove_cvref_t<decltype(std::declval<Receiver &>().get_stop_token())>;

} // namespace condy