/**
 * @file finish_handles.hpp
 * @brief Definitions of finish handle types for asynchronous operations.
 * @details This file defines various FinishHandle types for managing the
 * completion of asynchronous operations. Typically, the address of a
 * FinishHandle is set as the user_data of an async operation.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace condy {

template <CQEHandlerLike CQEHandler>
class OpFinishHandle : public OpFinishHandleBase {
public:
    using ReturnType = typename CQEHandler::ReturnType;

    template <typename... Args>
    OpFinishHandle(Args &&...args) : cqe_handler_(std::forward<Args>(args)...) {
        this->handle_func_ = handle_static_;
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

    ReturnType extract_result() noexcept {
        return cqe_handler_.extract_result();
    }

    void cancel(Runtime *runtime) noexcept {
        assert(runtime != nullptr);
        runtime->cancel(this);
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<OpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept {
        cqe_handler_.handle_cqe(cqe);
        assert(invoker_ != nullptr);
        (*invoker_)();
        return true;
    }

protected:
    Invoker *invoker_ = nullptr;
    CQEHandler cqe_handler_;
};

template <CQEHandlerLike CQEHandler, typename Func>
class MultiShotOpFinishHandle : public OpFinishHandle<CQEHandler> {
public:
    template <typename... Args>
    MultiShotOpFinishHandle(Func func, Args &&...args)
        : OpFinishHandle<CQEHandler>(std::forward<Args>(args)...),
          func_(std::move(func)) {
        this->handle_func_ = handle_static_;
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<MultiShotOpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept
    /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            this->cqe_handler_.handle_cqe(cqe);
            func_(this->cqe_handler_.extract_result());
            return false;
        } else {
            this->cqe_handler_.handle_cqe(cqe);
            assert(this->invoker_ != nullptr);
            (*this->invoker_)();
            return true;
        }
    }

protected:
    Func func_;
};

template <CQEHandlerLike CQEHandler, typename Func>
class ZeroCopyOpFinishHandle : public OpFinishHandle<CQEHandler> {
public:
    template <typename... Args>
    ZeroCopyOpFinishHandle(Func func, Args &&...args)
        : OpFinishHandle<CQEHandler>(std::forward<Args>(args)...),
          free_func_(std::move(func)) {
        this->handle_func_ = handle_static_;
    }

private:
    static bool handle_static_(void *data, io_uring_cqe *cqe) noexcept {
        auto *self = static_cast<ZeroCopyOpFinishHandle *>(data);
        return self->handle_impl_(cqe);
    }

    bool handle_impl_(io_uring_cqe *cqe) noexcept
    /* fake override */ {
        if (cqe->flags & IORING_CQE_F_MORE) {
            this->cqe_handler_.handle_cqe(cqe);
            assert(this->invoker_ != nullptr);
            (*this->invoker_)();
            return false;
        } else {
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                notify_(cqe->res);
                return true;
            } else {
                // Only one cqe means the operation is finished without
                // notification. This is rare but possible.
                // https://github.com/axboe/liburing/issues/1462
                this->cqe_handler_.handle_cqe(cqe);
                assert(this->invoker_ != nullptr);
                (*this->invoker_)();
                notify_(0);
                return true;
            }
        }
    }

    void notify_(int32_t res) noexcept {
        free_func_(res);
        delete this;
    }

protected:
    Func free_func_;
};

template <typename T>
constexpr bool is_nothrow_extract_result_v =
    noexcept(std::declval<T>().extract_result());

template <bool Cancel, HandleLike Handle> class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    void init(std::vector<Handle *> handles) noexcept {
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

    void cancel(Runtime *runtime) noexcept {
        if (!canceled_) {
            canceled_ = true;
            for (auto &handle : handles_) {
                handle->cancel(runtime);
            }
        }
    }

    ReturnType extract_result() noexcept(is_nothrow_extract_result_v<Handle>) {
        std::vector<ChildReturnType> result;
        result.reserve(handles_.size());
        for (size_t i = 0; i < handles_.size(); i++) {
            result.push_back(handles_[i]->extract_result());
        }
        return std::make_pair(std::move(order_), std::move(result));
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

private:
    void finish_(size_t idx) noexcept {
        size_t no = finished_count_++;
        order_[no] = idx;

        if constexpr (Cancel) {
            if (!canceled_) {
                canceled_ = true;
                auto *runtime = detail::Context::current().runtime();
                for (size_t i = 0; i < handles_.size(); i++) {
                    if (i != idx) {
                        handles_[i]->cancel(runtime);
                    }
                }
            }
        }

        if (no == handles_.size() - 1) {
            // All finished or canceled
            assert(invoker_ != nullptr);
            (*invoker_)();
            return;
        }
    }

private:
    struct FinishInvoker : public InvokerAdapter<FinishInvoker> {
        void invoke() noexcept { self_->finish_(no_); }
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

    ReturnType extract_result() noexcept(noexcept(Base::extract_result())) {
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
        // May throw out_of_range if the range is empty, which is expected and
        // should be handled by caller.
        auto idx = order.at(0);
        return std::make_pair(idx, std::move(results[idx]));
    }
};

template <bool Cancel, HandleLike... Handles> class ParallelFinishHandle {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Handles)>,
                                 std::tuple<typename Handles::ReturnType...>>;

    template <typename... HandlePtr> void init(HandlePtr... handles) noexcept {
        handles_ = std::make_tuple(handles...);
        foreach_set_invoker_();
    }

    void cancel(Runtime *runtime) noexcept {
        if (!canceled_) {
            canceled_ = true;
            std::apply(
                [runtime](auto *...handles) {
                    (handles->cancel(runtime), ...);
                },
                handles_);
        }
    }

    ReturnType
    extract_result() noexcept((is_nothrow_extract_result_v<Handles> && ...)) {
        auto result = std::apply(
            [](auto *...handle_ptrs) {
                return std::make_tuple(handle_ptrs->extract_result()...);
            },
            handles_);
        return std::make_pair(std::move(order_), std::move(result));
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

private:
    template <size_t I = 0> void foreach_set_invoker_() noexcept {
        if constexpr (I < sizeof...(Handles)) {
            auto *handle = std::get<I>(handles_);
            auto &invoker = std::get<I>(child_invokers_);
            invoker.self_ = this;
            handle->set_invoker(&invoker);
            foreach_set_invoker_<I + 1>();
        }
    }

    template <size_t SkipIdx, size_t I = 0>
    void foreach_call_cancel_(Runtime *runtime) noexcept {
        if constexpr (I < sizeof...(Handles)) {
            auto handle = std::get<I>(handles_);
            if constexpr (I != SkipIdx) {
                handle->cancel(runtime);
            }
            foreach_call_cancel_<SkipIdx, I + 1>(runtime);
        }
    }

    template <size_t Idx> void finish_() noexcept {
        size_t no = finished_count_++;
        order_[no] = Idx;

        if constexpr (Cancel) {
            if (!canceled_) {
                canceled_ = true;
                auto *runtime = detail::Context::current().runtime();
                foreach_call_cancel_<Idx>(runtime);
            }
        }

        if (no == sizeof...(Handles) - 1) {
            // All finished or canceled
            assert(invoker_ != nullptr);
            (*invoker_)();
        }
    }

private:
    template <size_t I>
    struct FinishInvoker : public InvokerAdapter<FinishInvoker<I>> {
        void invoke() noexcept { self_->template finish_<I>(); }
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

    ReturnType extract_result() noexcept(noexcept(Base::extract_result())) {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <HandleLike... Handles>
class WhenAnyFinishHandle : public ParallelAnyFinishHandle<Handles...> {
public:
    using Base = ParallelAnyFinishHandle<Handles...>;
    using ReturnType = std::variant<typename Handles::ReturnType...>;

    ReturnType extract_result() noexcept(noexcept(Base::extract_result())) {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return tuple_at_(results, order[0]);
    }

private:
    template <size_t Idx = 0>
    static auto tuple_at_(std::tuple<typename Handles::ReturnType...> &results,
                          size_t idx) {
        if constexpr (Idx < sizeof...(Handles)) {
            if (idx == Idx) {
                return ReturnType{std::in_place_index<Idx>,
                                  std::move(std::get<Idx>(results))};
            } else {
                return tuple_at_<Idx + 1>(results, idx);
            }
        } else {
            // Should not reach here, but we need to make compiler happy.
            // Throwing an exception will lead to wrong optimization.
            assert(false && "Index out of bounds");
            return ReturnType{std::in_place_index<0>,
                              std::move(std::get<0>(results))};
        }
    }
};

} // namespace condy
