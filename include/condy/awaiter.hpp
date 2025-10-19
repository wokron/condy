#pragma once

#include "condy/finish_handle.hpp"
#include <coroutine>
#include <cstddef>
#include <tuple>

namespace condy {

template <typename Handle, typename Awaiter> class RangedParallelAwaiter {
public:
    using HandleType = Handle;

    template <typename Range,
              typename = std::enable_if_t<!std::same_as<
                  std::remove_cvref_t<Range>, RangedParallelAwaiter>>>
    RangedParallelAwaiter(Range &&awaiters)
        : awaiters_(std::begin(awaiters), std::end(awaiters)) {}

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() {
        using ChildHandle = typename Awaiter::HandleType;
        std::vector<ChildHandle *> handles;
        handles.reserve(awaiters_.size());
        for (auto &awaiter : awaiters_) {
            awaiter.init_finish_handle();
            handles.push_back(awaiter.get_handle());
        }
        finish_handle_.init(std::move(handles));
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_on_finish(
            [h, this](typename HandleType::ReturnType r) {
                result_ = std::move(r);
                h.resume();
            });
    }

    typename Handle::ReturnType await_resume() { return std::move(result_); }

private:
    HandleType finish_handle_;
    typename HandleType::ReturnType result_;
    std::vector<Awaiter> awaiters_;
};

template <typename Awaiter>
using RangedWaitAllAwaiter = RangedParallelAwaiter<
    RangedWaitAllFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <typename Awaiter>
using RangedWaitOneAwaiter = RangedParallelAwaiter<
    RangedWaitOneFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <typename Handle, typename... Awaiters> class ParallelAwaiter {
public:
    using HandleType = Handle;

    ParallelAwaiter(Awaiters... awaiters) : awaiters_(std::move(awaiters)...) {}

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() {
        auto handles = foreach_init_finish_handle_();
        static_assert(std::tuple_size<decltype(handles)>::value ==
                          sizeof...(Awaiters),
                      "Number of handles must match number of awaiters");
        std::apply(
            [this](auto &&...handle_ptrs) {
                finish_handle_.init(handle_ptrs...);
            },
            handles);
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_on_finish(
            [h, this](typename HandleType::ReturnType r) {
                result_ = std::move(r);
                h.resume();
            });
    }

    typename Handle::ReturnType await_resume() { return std::move(result_); }

private:
    template <size_t Idx = 0> auto foreach_init_finish_handle_() {
        if constexpr (Idx < sizeof...(Awaiters)) {
            std::get<Idx>(awaiters_).init_finish_handle();
            return std::tuple_cat(
                std::make_tuple(std::get<Idx>(awaiters_).get_handle()),
                foreach_init_finish_handle_<Idx + 1>());
        } else {
            return std::tuple<>();
        }
    }

private:
    HandleType finish_handle_;
    typename HandleType::ReturnType result_;
    std::tuple<Awaiters...> awaiters_;
};

template <typename... Awaiter>
using WaitAllAwaiter =
    ParallelAwaiter<WaitAllFinishHandle<typename Awaiter::HandleType...>,
                    Awaiter...>;

template <typename... Awaiter>
using WaitOneAwaiter =
    ParallelAwaiter<WaitOneFinishHandle<typename Awaiter::HandleType...>,
                    Awaiter...>;

} // namespace condy