/**
 * @file awaiters.hpp
 * @brief Definitions of awaiter types for asynchronous operations.
 * @details This file defines a set of awaiter types used to represent and
 * manage asynchronous operations. These awaiters encapsulate the logic for
 * preparing, submitting, and resuming asynchronous tasks, and provide the
 * building blocks for composing complex asynchronous workflows.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/work_type.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace condy {

template <OpFinishHandleLike Handle> class HandleBox {
public:
    HandleBox(Handle h) : handle_(std::move(h)) {}

    Handle &get() { return handle_; }

private:
    Handle handle_;
};

template <typename Func, OpFinishHandleLike HandleBase>
class HandleBox<ZeroCopyMixin<Func, HandleBase>> {
public:
    using Handle = ZeroCopyMixin<Func, HandleBase>;
    HandleBox(Handle h) : handle_ptr_(new Handle(std::move(h))) {}

    Handle &get() { return *handle_ptr_; }

private:
    Handle *handle_ptr_;
};

template <OpFinishHandleLike Handle, PrepFuncLike Func> class OpAwaiterBase {
public:
    using HandleType = Handle;

    OpAwaiterBase(HandleBox<Handle> handle, Func func)
        : prep_func_(func), finish_handle_(std::move(handle)) {}
    OpAwaiterBase(OpAwaiterBase &&) = default;

    OpAwaiterBase(const OpAwaiterBase &) = delete;
    OpAwaiterBase &operator=(const OpAwaiterBase &) = delete;
    OpAwaiterBase &operator=(OpAwaiterBase &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_.get(); }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int flags) {
        auto &context = detail::Context::current();
        auto *ring = context.ring();

        context.runtime()->pend_work();

        io_uring_sqe *sqe = prep_func_(ring);
        assert(sqe && "prep_func must return a valid sqe");
        sqe->flags |= static_cast<uint8_t>(flags);
        io_uring_sqe_set_data(
            sqe, encode_work(&finish_handle_.get(), WorkType::Common));
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

protected:
    Func prep_func_;
    HandleBox<Handle> finish_handle_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler>
class [[nodiscard]] OpAwaiter
    : public OpAwaiterBase<OpFinishHandle<CQEHandler>, PrepFunc> {
public:
    using Base = OpAwaiterBase<OpFinishHandle<CQEHandler>, PrepFunc>;
    template <typename... Args>
    OpAwaiter(PrepFunc func, Args &&...args)
        : Base(HandleBox(
                   OpFinishHandle<CQEHandler>(std::forward<Args>(args)...)),
               std::move(func)) {}
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc>
class [[nodiscard]] MultiShotOpAwaiter
    : public OpAwaiterBase<MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>,
                           PrepFunc> {
public:
    using Base =
        OpAwaiterBase<MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>,
                      PrepFunc>;
    template <typename... Args>
    MultiShotOpAwaiter(PrepFunc func, MultiShotFunc multishot_func,
                       Args &&...args)
        : Base(HandleBox(MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>(
                   std::move(multishot_func), std::forward<Args>(args)...)),
               std::move(func)) {}
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc>
class [[nodiscard]] ZeroCopyOpAwaiter
    : public OpAwaiterBase<ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>,
                           PrepFunc> {
public:
    using Base =
        OpAwaiterBase<ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>, PrepFunc>;
    template <typename... Args>
    ZeroCopyOpAwaiter(PrepFunc func, FreeFunc free_func, Args &&...args)
        : Base(HandleBox(ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>(
                   std::move(free_func), std::forward<Args>(args)...)),
               std::move(func)) {}
};

template <unsigned int Flags, AwaiterLike Awaiter>
class [[nodiscard]] FlaggedOpAwaiter : public Awaiter {
public:
    using Base = Awaiter;
    FlaggedOpAwaiter(Awaiter awaiter) : Base(std::move(awaiter)) {}

    void register_operation(unsigned int flags) {
        Base::register_operation(flags | Flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        Base::init_finish_handle();
        Base::get_handle()->set_invoker(&h.promise());
        register_operation(0);
    }
};

template <HandleLike Handle, AwaiterLike Awaiter>
class [[nodiscard]] RangedParallelAwaiterBase {
public:
    using HandleType = Handle;

    RangedParallelAwaiterBase(std::vector<Awaiter> awaiters)
        : awaiters_(std::move(awaiters)) {}
    RangedParallelAwaiterBase(RangedParallelAwaiterBase &&) = default;

    RangedParallelAwaiterBase(const RangedParallelAwaiterBase &) = delete;
    RangedParallelAwaiterBase &
    operator=(const RangedParallelAwaiterBase &) = delete;
    RangedParallelAwaiterBase &operator=(RangedParallelAwaiterBase &&) = delete;

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
    void push(Awaiter awaiter) { awaiters_.push_back(std::move(awaiter)); }

protected:
    HandleType finish_handle_;
    std::vector<Awaiter> awaiters_;
};

/**
 * @brief Awaiter to wait for all operations in a range to complete.
 * @details An awaiter that waits for all operations in a range to complete.
 * Unlike @ref RangedWhenAllAwaiter, this awaiter will also return the order of
 * completion.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<std::vector<size_t>, std::vector<...>> A pair containing a
 * vector of completion orders and a vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedParallelAllAwaiter = RangedParallelAwaiterBase<
    RangedParallelAllFinishHandle<typename Awaiter::HandleType>, Awaiter>;

/**
 * @brief Awaiter to wait for any operation in a range to complete.
 * @details An awaiter that waits for any operation in a range to complete.
 * Unlike @ref RangedWhenAnyAwaiter, this awaiter will return the order of
 * completion of all operations and the result of each awaiter.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<std::vector<size_t>, std::vector<...>> A pair containing a
 * vector of completion orders and a vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedParallelAnyAwaiter = RangedParallelAwaiterBase<
    RangedParallelAnyFinishHandle<typename Awaiter::HandleType>, Awaiter>;

/**
 * @brief Awaiter to wait for all operations in a range to complete.
 * @details An awaiter that waits for all operations in a range to complete.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedWhenAllAwaiter = RangedParallelAwaiterBase<
    RangedWhenAllFinishHandle<typename Awaiter::HandleType>, Awaiter>;

/**
 * @brief Awaiter to wait for any operation in a range to complete.
 * @details An awaiter that waits for any operation in a range to complete.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::pair<size_t, ...> A pair containing the index of the completed
 * awaiter and its result.
 */
template <typename Awaiter>
using RangedWhenAnyAwaiter = RangedParallelAwaiterBase<
    RangedWhenAnyFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <unsigned int Flags, AwaiterLike Awaiter>
class [[nodiscard]] RangedLinkAwaiterBase
    : public RangedWhenAllAwaiter<Awaiter> {
public:
    using Base = RangedWhenAllAwaiter<Awaiter>;
    using Base::Base;

    void register_operation(unsigned int flags) {
        auto *ring = detail::Context::current().ring();
        ring->reserve_space(Base::awaiters_.size());
        for (int i = 0; i < Base::awaiters_.size() - 1; ++i) {
            Base::awaiters_[i].register_operation(flags | Flags);
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

/**
 * @brief Awaiter that links multiple operations in a range using IO_LINK.
 * @details An awaiter that links multiple operations in a range using IO_LINK.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedLinkAwaiter = RangedLinkAwaiterBase<IOSQE_IO_LINK, Awaiter>;

/**
 * @brief Awaiter that links multiple operations in a range using IO_HARDLINK.
 * @details An awaiter that links multiple operations in a range using
 * IO_HARDLINK.
 * @tparam Awaiter The type of the awaiters in the range.
 * @return std::vector<...> A vector of results from each awaiter.
 */
template <typename Awaiter>
using RangedHardLinkAwaiter = RangedLinkAwaiterBase<IOSQE_IO_HARDLINK, Awaiter>;

template <HandleLike Handle, AwaiterLike... Awaiters>
class [[nodiscard]] ParallelAwaiterBase {
public:
    using HandleType = Handle;

    ParallelAwaiterBase(Awaiters... awaiters)
        : awaiters_(std::move(awaiters)...) {}
    ParallelAwaiterBase(ParallelAwaiterBase &&) = default;
    template <typename ParallelAwaiter, AwaiterLike New>
    ParallelAwaiterBase(ParallelAwaiter &&aws, New new_awaiter)
        : awaiters_(std::tuple_cat(std::move(aws.awaiters_),
                                   std::make_tuple(std::move(new_awaiter)))) {}

    ParallelAwaiterBase(const ParallelAwaiterBase &) = delete;
    ParallelAwaiterBase &operator=(const ParallelAwaiterBase &) = delete;
    ParallelAwaiterBase &operator=(ParallelAwaiterBase &&) = delete;

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

    // Make awaiters_ accessible to all template instantiations
    template <HandleLike, AwaiterLike...> friend class ParallelAwaiterBase;
};

/**
 * @brief Awaiter to wait for all operations to complete in parallel.
 * @details An awaiter that waits for all operations to complete in parallel.
 * Unlike @ref WhenAllAwaiter, this awaiter will also return the order of
 * completion.
 * @tparam Awaiter The types of the awaiters.
 * @return std::pair<std::array<size_t, N>, std::tuple<...>> A pair containing
 * an array of completion orders and a tuple of results from each awaiter.
 */
template <typename... Awaiter>
using ParallelAllAwaiter = ParallelAwaiterBase<
    ParallelAllFinishHandle<typename Awaiter::HandleType...>, Awaiter...>;

/**
 * @brief Awaiter to wait for any operation to complete in parallel.
 * @details An awaiter that waits for any operation to complete in parallel.
 * Unlike @ref WhenAnyAwaiter, this awaiter will return the order of completion
 * of all operations and the result of each awaiter.
 * @tparam Awaiter The types of the awaiters.
 * @return std::pair<std::array<size_t, N>, std::tuple<...>> A pair containing
 * an array of completion orders and a tuple of results from each awaiter.
 */
template <typename... Awaiter>
using ParallelAnyAwaiter = ParallelAwaiterBase<
    ParallelAnyFinishHandle<typename Awaiter::HandleType...>, Awaiter...>;

/**
 * @brief Awaiter that waits for all operations to complete in parallel.
 * @details An awaiter that waits for all operations to complete in parallel.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter>
using WhenAllAwaiter =
    ParallelAwaiterBase<WhenAllFinishHandle<typename Awaiter::HandleType...>,
                        Awaiter...>;

/**
 * @brief Awaiter that waits for any operation to complete in parallel.
 * @details An awaiter that waits for any operation to complete in parallel.
 * @tparam Awaiter The types of the awaiters.
 * @return std::variant<...> A variant containing the result of the completed
 * awaiter.
 */
template <typename... Awaiter>
using WhenAnyAwaiter =
    ParallelAwaiterBase<WhenAnyFinishHandle<typename Awaiter::HandleType...>,
                        Awaiter...>;

template <unsigned int Flags, AwaiterLike... Awaiter>
class [[nodiscard]] LinkAwaiterBase : public WhenAllAwaiter<Awaiter...> {
public:
    using Base = WhenAllAwaiter<Awaiter...>;
    using Base::Base;

    void register_operation(unsigned int flags) {
        auto *ring = detail::Context::current().ring();
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
                .register_operation(Idx < sizeof...(Awaiter) - 1 ? flags | Flags
                                                                 : flags);
            foreach_register_operation_<Idx + 1>(flags);
        }
    }
};

/**
 * @brief Awaiter that links multiple operations using IO_LINK.
 * @details An awaiter that links multiple operations using IO_LINK.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter>
using LinkAwaiter = LinkAwaiterBase<IOSQE_IO_LINK, Awaiter...>;

/**
 * @brief Awaiter that links multiple operations using IO_HARDLINK.
 * @details An awaiter that links multiple operations using IO_HARDLINK.
 * @tparam Awaiter The types of the awaiters.
 * @return std::tuple<...> A tuple of results from each awaiter.
 */
template <typename... Awaiter>
using HardLinkAwaiter = LinkAwaiterBase<IOSQE_IO_HARDLINK, Awaiter...>;

} // namespace condy