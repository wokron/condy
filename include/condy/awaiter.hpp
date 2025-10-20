#pragma once

#include "condy/finish_handle.hpp"
#include <coroutine>
#include <cstddef>
#include <liburing.h>
#include <tuple>

namespace condy {

template <typename Func, typename... Args> class OpAwaiter {
public:
    using HandleType = OpFinishHandle;

    OpAwaiter(Func func, Args... args)
        : prep_func_(func), args_(std::make_tuple(std::move(args)...)) {}
    OpAwaiter(OpAwaiter &&) = default;

    OpAwaiter(const OpAwaiter &) = delete;
    OpAwaiter &operator=(const OpAwaiter &) = delete;
    OpAwaiter &operator=(OpAwaiter &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf handle, do nothing */ }

    void register_operation(unsigned int flags) {
        auto &context = Context::current();
        auto ring = context.get_ring();
        io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
        }
        assert(sqe != nullptr);
        std::apply([&](auto &&...args) { prep_func_(sqe, args...); }, args_);
        io_uring_sqe_set_data(sqe, &finish_handle_);
        io_uring_sqe_set_flags(sqe, flags);
    }

public:
    bool await_ready() { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_on_finish(
            [h, this](typename HandleType::ReturnType r) {
                result_ = std::move(r);
                h.resume();
            });
        register_operation(0);
    }

    int await_resume() { return result_; }

private:
    Func prep_func_;
    std::tuple<Args...> args_;
    OpFinishHandle finish_handle_;
    int result_;
};

template <typename Func, typename... Args>
auto build_op_awaiter(Func &&func, Args &&...args) {
    return OpAwaiter<std::decay_t<Func>, std::decay_t<Args>...>(
        std::forward<Func>(func), std::forward<Args>(args)...);
}

template <typename Handle, typename Awaiter> class RangedParallelAwaiter {
public:
    using HandleType = Handle;

    RangedParallelAwaiter(std::vector<Awaiter> awaiters)
        : awaiters_(std::move(awaiters)) {}
    RangedParallelAwaiter(RangedParallelAwaiter &&) = default;

    RangedParallelAwaiter(const RangedParallelAwaiter &) = delete;
    RangedParallelAwaiter &operator=(const RangedParallelAwaiter &) = delete;
    RangedParallelAwaiter &operator=(RangedParallelAwaiter &&) = delete;

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

    void register_operation(unsigned int flags) {
        for (auto &awaiter : awaiters_) {
            awaiter.register_operation(flags);
        }
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
        register_operation(0);
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
    ParallelAwaiter(ParallelAwaiter &&) = default;

    ParallelAwaiter(const ParallelAwaiter &) = delete;
    ParallelAwaiter &operator=(const ParallelAwaiter &) = delete;
    ParallelAwaiter &operator=(ParallelAwaiter &&) = delete;

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

    void register_operation(unsigned int flags) {
        foreach_register_operation_(flags);
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
        register_operation(0);
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

    template <size_t Idx = 0>
    void foreach_register_operation_(unsigned int flags) {
        if constexpr (Idx < sizeof...(Awaiters)) {
            std::get<Idx>(awaiters_).register_operation(flags);
            foreach_register_operation_<Idx + 1>(flags);
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