#pragma once

#include "condy/invoker.hpp"
#include <coroutine>

namespace condy {

template <typename T>
concept HandleLike = requires(T handle, Invoker *invoker) {
    typename T::ReturnType;
    { handle.set_invoker(invoker) } -> std::same_as<void>;
    { handle.extract_result() } -> std::same_as<typename T::ReturnType>;
    { handle.cancel() } -> std::same_as<void>;
};

template <typename T>
concept OpFinishHandleLike = HandleLike<T> && requires(T handle) {
    { handle.invoke() } -> std::same_as<void>;
    { handle.set_result(0, 0) } -> std::same_as<void>;
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

} // namespace condy