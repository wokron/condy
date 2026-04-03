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
#include <memory>
#include <tuple>

namespace condy {

template <OpFinishHandleLike Handle> class HandleBox {
public:
    HandleBox(Handle h) : handle_(std::move(h)) {}

    Handle &get() noexcept { return handle_; }

    void maybe_release() noexcept { /* No-op */ }

private:
    Handle handle_;
};

template <CQEHandlerLike CQEHandler, typename Func>
class HandleBox<ZeroCopyOpFinishHandle<CQEHandler, Func>> {
public:
    using Handle = ZeroCopyOpFinishHandle<CQEHandler, Func>;
    HandleBox(Handle h) : handle_ptr_(std::make_unique<Handle>(std::move(h))) {}
    HandleBox(const HandleBox &other) // Deep copy
        : handle_ptr_(std::make_unique<Handle>(*other.handle_ptr_)) {}

    Handle &get() noexcept { return *handle_ptr_; }

    void maybe_release() noexcept { handle_ptr_.release(); }

private:
    std::unique_ptr<Handle> handle_ptr_;
};

template <OpFinishHandleLike Handle, PrepFuncLike Func> class OpAwaiterBase {
public:
    using HandleType = Handle;

    template <typename... Args>
    OpAwaiterBase(Func func, Args &&...args)
        : prep_func_(std::move(func)),
          finish_handle_(Handle(std::forward<Args>(args)...)) {}

public:
    HandleType *get_handle() noexcept { return &finish_handle_.get(); }

    void init_finish_handle() noexcept { /* Leaf node, no-op */ }

    void register_operation(unsigned int flags) noexcept {
        auto &context = detail::Context::current();
        auto *ring = context.ring();

        context.runtime()->pend_work();

        io_uring_sqe *sqe = prep_func_(ring);
        assert(sqe && "prep_func must return a valid sqe");
        io_uring_sqe_set_flags(sqe, sqe->flags | flags);
        auto *work = encode_work(&finish_handle_.get(), WorkType::Common);
        io_uring_sqe_set_data(sqe, work);
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
        init_finish_handle();
        finish_handle_.get().set_invoker(&h.promise());
        register_operation(0);
    }

    auto await_resume() noexcept {
        auto result = finish_handle_.get().extract_result();
        finish_handle_.maybe_release();
        return result;
    }

protected:
    Func prep_func_;
    HandleBox<Handle> finish_handle_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler>
using OpAwaiter = OpAwaiterBase<OpFinishHandle<CQEHandler>, PrepFunc>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc>
using MultiShotOpAwaiter =
    OpAwaiterBase<MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>, PrepFunc>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc>
using ZeroCopyOpAwaiter =
    OpAwaiterBase<ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>, PrepFunc>;

template <unsigned int Flags, AwaiterLike Awaiter>
class [[nodiscard]] FlaggedOpAwaiter : public Awaiter {
public:
    using Base = Awaiter;
    FlaggedOpAwaiter(Awaiter awaiter) : Base(std::move(awaiter)) {}

    void register_operation(unsigned int flags) noexcept {
        Base::register_operation(flags | Flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
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

public:
    HandleType *get_handle() noexcept { return &finish_handle_; }

    void init_finish_handle() noexcept {
        using ChildHandle = typename Awaiter::HandleType;
        std::vector<ChildHandle *> handles;
        handles.reserve(awaiters_.size());
        for (auto &awaiter : awaiters_) {
            awaiter.init_finish_handle();
            handles.push_back(awaiter.get_handle());
        }
        finish_handle_.init(std::move(handles));
    }

    void register_operation(unsigned int flags) noexcept {
        for (auto &awaiter : awaiters_) {
            awaiter.register_operation(flags);
        }
    }

public:
    bool await_ready() const noexcept { return awaiters_.empty(); }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

    typename Handle::ReturnType
    await_resume() noexcept(is_nothrow_extract_result_v<Handle>) {
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
 * @throws std::out_of_range if the range is empty.
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

    void register_operation(unsigned int flags) noexcept {
        auto *ring = detail::Context::current().ring();
        ring->reserve_space(Base::awaiters_.size());
        for (int i = 0; i < Base::awaiters_.size() - 1; ++i) {
            Base::awaiters_[i].register_operation(flags | Flags);
        }
        Base::awaiters_.back().register_operation(flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
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
    template <typename ParallelAwaiter, AwaiterLike New>
    ParallelAwaiterBase(ParallelAwaiter &&aws, New &&new_awaiter)
        : awaiters_(
              std::tuple_cat(std::move(aws.awaiters_),
                             std::make_tuple(std::forward<New>(new_awaiter)))) {
    }

public:
    HandleType *get_handle() noexcept { return &finish_handle_; }

    void init_finish_handle() noexcept {
        std::apply(
            [this](auto &&...awaiters) {
                (awaiters.init_finish_handle(), ...);
                finish_handle_.init(awaiters.get_handle()...);
            },
            awaiters_);
    }

    void register_operation(unsigned int flags) noexcept {
        std::apply(
            [flags](auto &&...awaiters) {
                (awaiters.register_operation(flags), ...);
            },
            awaiters_);
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

    typename Handle::ReturnType
    await_resume() noexcept(is_nothrow_extract_result_v<Handle>) {
        return finish_handle_.extract_result();
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

    void register_operation(unsigned int flags) noexcept {
        auto *ring = detail::Context::current().ring();
        ring->reserve_space(sizeof...(Awaiter));
        foreach_register_operation_(flags);
    }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
        Base::init_finish_handle();
        Base::finish_handle_.set_invoker(&h.promise());
        register_operation(0);
    }

private:
    template <size_t Idx = 0>
    void foreach_register_operation_(unsigned int flags) noexcept {
        if constexpr (Idx < sizeof...(Awaiter)) {
            auto &awaiter = std::get<Idx>(Base::awaiters_);
            if constexpr (Idx < sizeof...(Awaiter) - 1) {
                awaiter.register_operation(flags | Flags);
            } else {
                awaiter.register_operation(flags);
            }
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