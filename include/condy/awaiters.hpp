#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"
#include <coroutine>
#include <cstddef>
#include <tuple>

namespace condy {

template <typename Handle> class HandleBox {
public:
    HandleBox(Handle h) : handle_(std::move(h)) {}

    Handle &get() { return handle_; }

private:
    Handle handle_;
};

template <typename Func, typename HandleBase>
class HandleBox<ZeroCopyMixin<Func, HandleBase>> {
public:
    using Handle = ZeroCopyMixin<Func, HandleBase>;
    HandleBox(Handle h) : handle_ptr_(new Handle(std::move(h))) {}

    Handle &get() { return *handle_ptr_; }

private:
    Handle *handle_ptr_;
};

template <> class HandleBox<TimerFinishHandle> {
public:
    HandleBox(TimerFinishHandle *h) : handle_ptr_(h) {}

    TimerFinishHandle &get() { return *handle_ptr_; }

private:
    TimerFinishHandle *handle_ptr_;
};

template <typename Handle, typename Func, typename... Args>
class OpAwaiterBase {
public:
    using HandleType = Handle;

    OpAwaiterBase(HandleBox<Handle> handle, Func func, Args... args)
        : finish_handle_(std::move(handle)), prep_func_(func),
          args_(std::make_tuple(std::move(args)...)) {}
    OpAwaiterBase(OpAwaiterBase &&) = default;

    OpAwaiterBase(const OpAwaiterBase &) = delete;
    OpAwaiterBase &operator=(const OpAwaiterBase &) = delete;
    OpAwaiterBase &operator=(OpAwaiterBase &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_.get(); }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int flags) {
        auto &context = Context::current();
        auto *ring = context.ring();
        finish_handle_.get().set_ring(ring);
        ring->register_op(
            [&](io_uring_sqe *sqe) {
                std::apply(
                    [&](auto &&...args) {
                        prep_func_(sqe, std::forward<decltype(args)>(args)...);
                    },
                    args_);
                io_uring_sqe_set_flags(sqe, flags | this->flags_);
                if (bgid_ >= 0) {
                    sqe->buf_group = bgid_;
                }
            },
            &finish_handle_.get());
    }

public:
    bool await_ready() { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.get().set_invoker(&h.promise());
        register_operation(0);
    }

    auto await_resume() { return finish_handle_.get().extract_result(); }

public:
    void add_flags(unsigned int flags) { flags_ |= flags; }

    void set_bgid(int bgid) { bgid_ = bgid; }

protected:
    Func prep_func_;
    std::tuple<Args...> args_;
    HandleBox<Handle> finish_handle_;
    unsigned int flags_ = 0;
    int bgid_ = -1;
};

template <typename Func, typename... Args>
class [[nodiscard]] OpAwaiter
    : public OpAwaiterBase<OpFinishHandle, Func, Args...> {
public:
    using Base = OpAwaiterBase<OpFinishHandle, Func, Args...>;
    OpAwaiter(Func func, Args... args)
        : Base(OpFinishHandle(), func, args...) {}
};

template <typename MultiShotFunc, typename Func, typename... Args>
class [[nodiscard]] MultiShotOpAwaiter
    : public OpAwaiterBase<MultiShotOpFinishHandle<MultiShotFunc>, Func,
                           Args...> {
public:
    using Base =
        OpAwaiterBase<MultiShotOpFinishHandle<MultiShotFunc>, Func, Args...>;
    MultiShotOpAwaiter(MultiShotFunc multishot_func, Func func, Args... args)
        : Base(
              MultiShotOpFinishHandle<MultiShotFunc>(std::move(multishot_func)),
              func, args...) {}
};

template <typename Func, typename... Args>
class [[nodiscard]] SelectBufferOpAwaiter
    : public OpAwaiterBase<SelectBufferOpFinishHandle, Func, Args...> {
public:
    using Base = OpAwaiterBase<SelectBufferOpFinishHandle, Func, Args...>;
    SelectBufferOpAwaiter(detail::ProvidedBufferPoolImplPtr buffers_impl,
                          Func func, Args... args)
        : Base(SelectBufferOpFinishHandle(std::move(buffers_impl)), func,
               args...) {}
};

template <typename MultiShotFunc, typename Func, typename... Args>
class [[nodiscard]] MultiShotSelectBufferOpAwaiter
    : public OpAwaiterBase<MultiShotSelectBufferOpFinishHandle<MultiShotFunc>,
                           Func, Args...> {
public:
    using Base =
        OpAwaiterBase<MultiShotSelectBufferOpFinishHandle<MultiShotFunc>, Func,
                      Args...>;
    MultiShotSelectBufferOpAwaiter(MultiShotFunc multishot_func,
                                   detail::ProvidedBufferPoolImplPtr buffers_impl,
                                   Func func, Args... args)
        : Base(MultiShotSelectBufferOpFinishHandle<MultiShotFunc>(
                   std::move(multishot_func), std::move(buffers_impl)),
               func, args...) {}
};

template <typename FreeFunc, typename Func, typename... Args>
class [[nodiscard]] ZeroCopyOpAwaiter
    : public OpAwaiterBase<ZeroCopyOpFinishHandle<FreeFunc>, Func, Args...> {
public:
    using Base = OpAwaiterBase<ZeroCopyOpFinishHandle<FreeFunc>, Func, Args...>;
    ZeroCopyOpAwaiter(FreeFunc free_func, Func func, Args... args)
        : Base(ZeroCopyOpFinishHandle<FreeFunc>(std::move(free_func)), func,
               args...) {}
};

template <typename Func, typename... Args>
class [[nodiscard]] TimerOpAwaiter
    : public OpAwaiterBase<TimerFinishHandle, Func, Args...> {
public:
    using Base = OpAwaiterBase<TimerFinishHandle, Func, Args...>;
    TimerOpAwaiter(TimerFinishHandle *timer_handle, Func func, Args... args)
        : Base(HandleBox<TimerFinishHandle>(timer_handle), func, args...) {}
};

template <typename Handle, typename Awaiter>
class [[nodiscard]] RangedParallelAwaiter {
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
        finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

    typename Handle::ReturnType await_resume() {
        return finish_handle_.extract_result();
    }

public:
    void append_awaiter(Awaiter awaiter) {
        awaiters_.push_back(std::move(awaiter));
    }

protected:
    HandleType finish_handle_;
    std::vector<Awaiter> awaiters_;
};

template <bool Cancel, typename Awaiter>
using RangedParallelAwaiterWrapper = RangedParallelAwaiter<
    RangedParallelFinishHandle<Cancel, typename Awaiter::HandleType>, Awaiter>;

template <typename Awaiter>
using RangedWaitAllAwaiter = RangedParallelAwaiter<
    RangedWaitAllFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <typename Awaiter>
using RangedWaitOneAwaiter = RangedParallelAwaiter<
    RangedWaitOneFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <typename Awaiter>
class [[nodiscard]] RangedLinkAwaiter : public RangedWaitAllAwaiter<Awaiter> {
public:
    using Base = RangedWaitAllAwaiter<Awaiter>;
    using Base::Base;

    void register_operation(unsigned int flags) {
        auto *ring = Context::current().ring();
        ring->reserve_space(Base::awaiters_.size());
        for (int i = 0; i < Base::awaiters_.size() - 1; ++i) {
            Base::awaiters_[i].register_operation(flags | IOSQE_IO_LINK);
        }
        Base::awaiters_.back().register_operation(flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        Base::init_finish_handle();
        Base::finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }
};

template <typename Handle, typename... Awaiters>
class [[nodiscard]] ParallelAwaiter {
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
        finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

    typename Handle::ReturnType await_resume() {
        return finish_handle_.extract_result();
    }

public:
    auto awaiters() && { return std::move(awaiters_); }

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

protected:
    HandleType finish_handle_;
    std::tuple<Awaiters...> awaiters_;
};

template <bool Cancel, typename... Awaiter>
using ParallelAwaiterWrapper = ParallelAwaiter<
    ParallelFinishHandle<Cancel, typename Awaiter::HandleType...>, Awaiter...>;

template <typename... Awaiter>
using WaitAllAwaiter =
    ParallelAwaiter<WaitAllFinishHandle<typename Awaiter::HandleType...>,
                    Awaiter...>;

template <typename... Awaiter>
using WaitOneAwaiter =
    ParallelAwaiter<WaitOneFinishHandle<typename Awaiter::HandleType...>,
                    Awaiter...>;

template <typename... Awaiter>
class [[nodiscard]] LinkAwaiter : public WaitAllAwaiter<Awaiter...> {
public:
    using Base = WaitAllAwaiter<Awaiter...>;
    using Base::Base;

    void register_operation(unsigned int flags) {
        auto *ring = Context::current().ring();
        ring->reserve_space(sizeof...(Awaiter));
        foreach_register_operation_(flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        Base::init_finish_handle();
        Base::finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

private:
    template <size_t Idx = 0>
    void foreach_register_operation_(unsigned int flags) {
        if constexpr (Idx < sizeof...(Awaiter)) {
            std::get<Idx>(Base::awaiters_)
                .register_operation(Idx < sizeof...(Awaiter) - 1
                                        ? flags | IOSQE_IO_LINK
                                        : flags);
            foreach_register_operation_<Idx + 1>(flags);
        }
    }
};

} // namespace condy