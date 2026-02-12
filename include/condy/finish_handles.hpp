/**
 * @file finish_handles.hpp
 * @brief Definitions of finish handle types for asynchronous operations.
 * @details This file defines various FinishHandle types for managing the
 * completion of asynchronous operations. Typically, the address of a
 * FinishHandle is set as the user_data of an async operation.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/work_type.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <tuple>
#include <variant>
#include <vector>

namespace condy {

class Ring;

class OpFinishHandle : public InvokerAdapter<OpFinishHandle, WorkInvoker> {
public:
    using ReturnType = int32_t;
    struct Action {
        bool queue_work;
        bool op_finish;
    };
    using HandleCQEFunc = Action (*)(void *, io_uring_cqe *);

    void cancel() {
        auto *ring = detail::Context::current().ring();
        io_uring_sqe *sqe = ring->get_sqe();
        io_uring_prep_cancel(sqe, this, 0);
        io_uring_sqe_set_data(sqe, encode_work(nullptr, WorkType::Ignore));
        io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    }

    Action handle_cqe(io_uring_cqe *cqe) { return handle_func_(this, cqe); }

    Action handle_cqe_impl(io_uring_cqe *cqe) {
        res_ = cqe->res;
        return {.queue_work = true, .op_finish = true};
    }

    void invoke() {
        assert(invoker_ != nullptr);
        (*invoker_)();
    }

    ReturnType extract_result() { return res_; }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

private:
    static Action handle_cqe_static_(void *data, io_uring_cqe *cqe) {
        auto *self = static_cast<OpFinishHandle *>(data);
        return self->handle_cqe_impl(cqe);
    }

protected:
    HandleCQEFunc handle_func_ = handle_cqe_static_;
    Invoker *invoker_ = nullptr;
    int32_t res_ = -ENOTRECOVERABLE; // Internal error if not set
};

template <typename Func, OpFinishHandleLike HandleBase>
class MultiShotMixin : public HandleBase {
public:
    template <typename... Args>
    MultiShotMixin(Func func, Args &&...args)
        : HandleBase(std::forward<Args>(args)...), func_(std::move(func)) {
        this->handle_func_ = handle_cqe_static_;
    }

    OpFinishHandle::Action
    handle_cqe_impl(io_uring_cqe *cqe) /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            HandleBase::handle_cqe_impl(cqe);
            func_(HandleBase::extract_result());
            return {.queue_work = false, .op_finish = false};
        } else {
            HandleBase::handle_cqe_impl(cqe);
            return {.queue_work = true, .op_finish = true};
        }
    }

private:
    static OpFinishHandle::Action handle_cqe_static_(void *data,
                                                     io_uring_cqe *cqe) {
        auto *self = static_cast<MultiShotMixin *>(data);
        return self->handle_cqe_impl(cqe);
    }

protected:
    Func func_;
};

template <typename MultiShotFunc>
using MultiShotOpFinishHandle = MultiShotMixin<MultiShotFunc, OpFinishHandle>;

template <typename Func, OpFinishHandleLike HandleBase>
class ZeroCopyMixin : public HandleBase {
public:
    template <typename... Args>
    ZeroCopyMixin(Func func, Args &&...args)
        : HandleBase(std::forward<Args>(args)...), free_func_(std::move(func)) {
        this->func_ = invoke_static_;
        this->handle_func_ = handle_cqe_static_;
    }

    void invoke() /* fake override */ {
        assert(this->invoker_ != nullptr);
        (*this->invoker_)();
        resumed_ = true;
        // Invocation of free_func_ should be delayed until the operation is
        // finished since user may adjust the behavior of free_func_ based on
        // the result of the operation.
        maybe_free_();
    }

    OpFinishHandle::Action
    handle_cqe_impl(io_uring_cqe *cqe) /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            HandleBase::handle_cqe_impl(cqe);
            return {.queue_work = true, .op_finish = false};
        } else {
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                notify_(cqe->res);
                return {.queue_work = false, .op_finish = true};
            } else {
                // Only one cqe means the operation is finished without
                // notification. This is rare but possible.
                // https://github.com/axboe/liburing/issues/1462
                notify_(0);
                HandleBase::handle_cqe_impl(cqe);
                return {.queue_work = true, .op_finish = true};
            }
        }
    }

private:
    void maybe_free_() {
        if (resumed_ && notified_) {
            free_func_(notify_res_);
            delete this;
        }
    }

    void notify_(int32_t res) {
        assert(res != -ENOTRECOVERABLE);
        notify_res_ = res;
        notified_ = true;
        maybe_free_();
    }

    static void invoke_static_(void *data) {
        auto *self = static_cast<ZeroCopyMixin *>(data);
        self->invoke();
    }

    static OpFinishHandle::Action handle_cqe_static_(void *data,
                                                     io_uring_cqe *cqe) {
        auto *self = static_cast<ZeroCopyMixin *>(data);
        return self->handle_cqe_impl(cqe);
    }

protected:
    Func free_func_;
    int32_t notify_res_ = -ENOTRECOVERABLE;
    // Use these flags to handle race between invoke and notify
    bool resumed_ = false;
    bool notified_ = false;
};

template <typename FreeFunc>
using ZeroCopyOpFinishHandle = ZeroCopyMixin<FreeFunc, OpFinishHandle>;

template <BufferRingLike Br, OpFinishHandleLike HandleBase>
class SelectBufferMixin : public HandleBase {
public:
    using ReturnType = std::pair<int, typename Br::ReturnType>;

    template <typename... Args>
    SelectBufferMixin(Br *buffers, Args &&...args)
        : HandleBase(std::forward<Args>(args)...), buffers_(buffers) {
        this->handle_func_ = handle_cqe_static_;
    }

    OpFinishHandle::Action
    handle_cqe_impl(io_uring_cqe *cqe) /* fake override */ {
        HandleBase::handle_cqe_impl(cqe);
        this->flags_ = cqe->flags;
        return {.queue_work = true, .op_finish = true};
    }

    ReturnType extract_result() {
        int res = this->res_;
        return std::make_pair(
            res, buffers_->handle_finish(this->res_, this->flags_));
    }

private:
    static OpFinishHandle::Action handle_cqe_static_(void *data,
                                                     io_uring_cqe *cqe) {
        auto *self = static_cast<SelectBufferMixin *>(data);
        return self->handle_cqe_impl(cqe);
    }

protected:
    uint32_t flags_ = 0;
    Br *buffers_;
};

template <BufferRingLike Br>
using SelectBufferOpFinishHandle = SelectBufferMixin<Br, OpFinishHandle>;

template <typename MultiShotFunc, BufferRingLike Br>
using MultiShotSelectBufferOpFinishHandle =
    MultiShotMixin<MultiShotFunc, SelectBufferMixin<Br, OpFinishHandle>>;

template <bool Cancel, HandleLike Handle> class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    void init(std::vector<Handle *> handles) {
        handles_ = std::move(handles);
        child_invokers_.resize(handles_.size());
        for (size_t i = 0; i < handles_.size(); i++) {
            auto *handle = handles_[i];
            auto &invoker = child_invokers_[i];
            invoker.self_ = this;
            invoker.no_ = i;
            handle->set_invoker(&invoker);
        }
        order_.resize(handles_.size());
    }

    void cancel() {
        if (!canceled_) {
            canceled_ = true;
            for (auto &handle : handles_) {
                handle->cancel();
            }
        }
    }

    ReturnType extract_result() {
        std::vector<ChildReturnType> result;
        result.reserve(handles_.size());
        for (size_t i = 0; i < handles_.size(); i++) {
            result.push_back(handles_[i]->extract_result());
        }
        return std::make_pair(std::move(order_), std::move(result));
    }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

private:
    void finish_(size_t idx) {
        size_t no = finished_count_++;
        order_[no] = idx;

        if constexpr (Cancel) {
            if (!canceled_) {
                canceled_ = true;
                for (size_t i = 0; i < handles_.size(); i++) {
                    if (i != idx) {
                        handles_[i]->cancel();
                    }
                }
            }
        }

        if (no == handles_.size() - 1) {
            // All finished or canceled
            (*invoker_)();
            return;
        }
    }

private:
    struct FinishInvoker : public InvokerAdapter<FinishInvoker> {
        void invoke() { self_->finish_(no_); }
        RangedParallelFinishHandle *self_;
        size_t no_;
    };

    size_t finished_count_ = 0;
    bool canceled_ = false;
    std::vector<Handle *> handles_ = {};
    std::vector<FinishInvoker> child_invokers_;
    std::vector<size_t> order_;
    Invoker *invoker_ = nullptr;
};

template <HandleLike Handle>
using RangedParallelAllFinishHandle = RangedParallelFinishHandle<false, Handle>;

template <HandleLike Handle>
using RangedParallelAnyFinishHandle = RangedParallelFinishHandle<true, Handle>;

template <HandleLike Handle>
class RangedWhenAllFinishHandle : public RangedParallelAllFinishHandle<Handle> {
public:
    using Base = RangedParallelAllFinishHandle<Handle>;
    using ReturnType = std::vector<typename Handle::ReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <HandleLike Handle>
class RangedWhenAnyFinishHandle : public RangedParallelAnyFinishHandle<Handle> {
public:
    using Base = RangedParallelAnyFinishHandle<Handle>;
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType = std::pair<size_t, ChildReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return std::make_pair(order[0], std::move(results[order[0]]));
    }
};

template <bool Cancel, HandleLike... Handles> class ParallelFinishHandle {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Handles)>,
                                 std::tuple<typename Handles::ReturnType...>>;

    template <typename... HandlePtr> void init(HandlePtr... handles) {
        handles_ = std::make_tuple(handles...);
        foreach_set_invoker_();
    }

    void cancel() {
        if (!canceled_) {
            canceled_ = true;
            constexpr size_t SkipIdx = std::numeric_limits<size_t>::max();
            foreach_call_cancel_<SkipIdx>();
        }
    }

    ReturnType extract_result() {
        auto result = std::apply(
            [](auto *...handle_ptrs) {
                return std::make_tuple(handle_ptrs->extract_result()...);
            },
            handles_);
        return std::make_pair(std::move(order_), std::move(result));
    }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

private:
    template <size_t I = 0> void foreach_set_invoker_() {
        if constexpr (I < sizeof...(Handles)) {
            auto *handle = std::get<I>(handles_);
            auto &invoker = std::get<I>(child_invokers_);
            invoker.self_ = this;
            handle->set_invoker(&invoker);
            foreach_set_invoker_<I + 1>();
        }
    }

    template <size_t SkipIdx, size_t I = 0> void foreach_call_cancel_() {
        if constexpr (I < sizeof...(Handles)) {
            auto handle = std::get<I>(handles_);
            if constexpr (I != SkipIdx) {
                handle->cancel();
            }
            foreach_call_cancel_<SkipIdx, I + 1>();
        }
    }

    template <size_t Idx> void finish_() {
        size_t no = finished_count_++;
        order_[no] = Idx;

        if constexpr (Cancel) {
            if (!canceled_) {
                canceled_ = true;
                foreach_call_cancel_<Idx>();
            }
        }

        if (no == sizeof...(Handles) - 1) {
            // All finished or canceled
            (*invoker_)();
        }
    }

private:
    template <size_t I>
    struct FinishInvoker : public InvokerAdapter<FinishInvoker<I>> {
        void invoke() { self_->template finish_<I>(); }
        ParallelFinishHandle *self_;
    };

    template <typename... input_t>
    using tuple_cat_t = decltype(std::tuple_cat(std::declval<input_t>()...));

    template <size_t I, typename Arg, typename... Args> struct helper {
        using type = tuple_cat_t<std::tuple<FinishInvoker<I>>,
                                 typename helper<I + 1, Args...>::type>;
    };

    template <size_t I, typename Arg> struct helper<I, Arg> {
        using type = std::tuple<FinishInvoker<I>>;
    };

    using InvokerTupleType = typename helper<0, Handles...>::type;

    size_t finished_count_ = 0;
    bool canceled_ = false;
    std::tuple<Handles *...> handles_;
    InvokerTupleType child_invokers_;
    std::array<size_t, sizeof...(Handles)> order_;
    Invoker *invoker_ = nullptr;
};

template <HandleLike... Handles>
using ParallelAllFinishHandle = ParallelFinishHandle<false, Handles...>;

template <HandleLike... Handles>
using ParallelAnyFinishHandle = ParallelFinishHandle<true, Handles...>;

template <HandleLike... Handles>
class WhenAllFinishHandle : public ParallelAllFinishHandle<Handles...> {
public:
    using Base = ParallelAllFinishHandle<Handles...>;
    using ReturnType = std::tuple<typename Handles::ReturnType...>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <HandleLike... Handles>
class WhenAnyFinishHandle : public ParallelAnyFinishHandle<Handles...> {
public:
    using Base = ParallelAnyFinishHandle<Handles...>;
    using ReturnType = std::variant<typename Handles::ReturnType...>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return tuple_at_(std::move(results), order[0]);
    }

private:
    template <size_t Idx = 0>
    static std::variant<typename Handles::ReturnType...>
    tuple_at_(std::tuple<typename Handles::ReturnType...> results, size_t idx) {
        if constexpr (Idx < sizeof...(Handles)) {
            if (idx == Idx) {
                return std::variant<typename Handles::ReturnType...>{
                    std::in_place_index<Idx>,
                    std::move(std::get<Idx>(results))};
            } else {
                return tuple_at_<Idx + 1>(std::move(results), idx);
            }
        } else {
            // Should not reach here, but we need to make compiler happy.
            // Throwing an exception will lead to wrong optimization.
            assert(false && "Index out of bounds");
            return std::variant<typename Handles::ReturnType...>{
                std::in_place_index<0>, std::move(std::get<0>(results))};
        }
    }
};

} // namespace condy
