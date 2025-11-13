#pragma once

#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/utils.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

namespace condy {

struct Ring;

class OpFinishHandle : public InvokerAdapter<OpFinishHandle, WorkInvoker> {
public:
    using ReturnType = int;
    using MultiShotFunc = void (*)(void *);

    OpFinishHandle() = default;

    void cancel() {
        assert(ring_ != nullptr);
        ring_->cancel_op(this);
    }

    void set_result(int res, int flags) {
        res_ = res;
        flags_ = flags;
    }

    void operator()() { (*invoker_)(); }

    int extract_result() { return res_; }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void set_ring(Ring *ring) { ring_ = ring; }

    void multishot() {
        assert(multishot_func_ != nullptr);
        multishot_func_(this);
    }

protected:
    MultiShotFunc multishot_func_ = nullptr;
    Ring *ring_ = nullptr;
    int res_;
    int flags_;
    Invoker *invoker_ = nullptr;
};

template <typename Func, typename HandleBase>
class MultiShotMixin : public HandleBase {
public:
    template <typename... Args>
    MultiShotMixin(Func func, Args &&...args)
        : HandleBase(std::forward<Args>(args)...), func_(std::move(func)) {
        this->multishot_func_ = &MultiShotMixin::invoke_multishot_;
    }

private:
    static void invoke_multishot_(void *data) {
        auto *self = static_cast<MultiShotMixin<Func, HandleBase> *>(data);
        self->func_(self->extract_result());
    }

private:
    Func func_;
};

template <typename MultiShotFunc>
using MultiShotOpFinishHandle = MultiShotMixin<MultiShotFunc, OpFinishHandle>;

template <typename Condition, typename Handle>
class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    void init(std::vector<Handle *> handles) {
        finished_count_.emplace(0);
        canceled_count_.emplace(0);
        outstanding_count_.emplace(handles.size());
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
        if (canceled_count_.get()++ == 0) {
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
        size_t no = finished_count_.get()++;
        order_[no] = idx;

        if (cancel_checker_(idx) && canceled_count_.get()++ == 0) {
            for (size_t i = 0; i < handles_.size(); i++) {
                if (i != idx) {
                    handles_[i]->cancel();
                }
            }
        }

        if (--outstanding_count_.get() == 0) {
            // All finished or canceled
            (*invoker_)();
        }
    }

private:
    struct FinishInvoker : public InvokerAdapter<FinishInvoker> {
        void operator()() { self_->finish_(no_); }
        RangedParallelFinishHandle *self_;
        size_t no_;
    };

    Uninitialized<std::atomic_size_t> finished_count_;
    Uninitialized<std::atomic_size_t> outstanding_count_;
    Uninitialized<std::atomic_size_t> canceled_count_;
    std::vector<Handle *> handles_ = {};
    std::vector<FinishInvoker> child_invokers_;
    Condition cancel_checker_ = {};
    std::vector<size_t> order_;
    Invoker *invoker_ = nullptr;
};

struct WaitAllCancelCondition {
    template <typename... Args> bool operator()(Args &&...args) const {
        return false;
    }
};

struct WaitOneCancelCondition {
    template <typename... Args> bool operator()(Args &&...args) const {
        return true;
    }
};

template <typename Handle>
class RangedWaitAllFinishHandle
    : public RangedParallelFinishHandle<WaitAllCancelCondition, Handle> {
public:
    using Base = RangedParallelFinishHandle<WaitAllCancelCondition, Handle>;
    using ReturnType = std::vector<typename Handle::ReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <typename Handle>
class RangedWaitOneFinishHandle
    : public RangedParallelFinishHandle<WaitOneCancelCondition, Handle> {
public:
    using Base = RangedParallelFinishHandle<WaitOneCancelCondition, Handle>;
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType = std::pair<size_t, ChildReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return std::make_pair(order[0], std::move(results[order[0]]));
    }
};

template <typename Condition, typename... Handles> class ParallelFinishHandle {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Handles)>,
                                 std::tuple<typename Handles::ReturnType...>>;

    template <typename... HandlePtr> void init(HandlePtr... handles) {
        finished_count_.emplace(0);
        outstanding_count_.emplace(sizeof...(Handles));
        canceled_count_.emplace(0);
        handles_ = std::make_tuple(handles...);
        foreach_set_invoker_();
    }

    void cancel() {
        if (canceled_count_.get()++ == 0) {
            constexpr size_t SkipIdx = std::numeric_limits<size_t>::max();
            foreach_call_cancel_<SkipIdx>();
        }
    }

    ReturnType extract_result() {
        auto result = std::apply(
            [this](auto *...handle_ptrs) {
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
        size_t no = finished_count_.get()++;
        order_[no] = Idx;

        if (cancel_checker_(Idx) && canceled_count_.get()++ == 0) {
            foreach_call_cancel_<Idx>();
        }

        if (--outstanding_count_.get() == 0) {
            // All finished or canceled
            (*invoker_)();
        }
    }

private:
    template <size_t I>
    struct FinishInvoker : public InvokerAdapter<FinishInvoker<I>> {
        void operator()() { self_->template finish_<I>(); }
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

    Uninitialized<std::atomic_size_t> finished_count_;
    Uninitialized<std::atomic_size_t> outstanding_count_;
    Uninitialized<std::atomic_size_t> canceled_count_;
    std::tuple<Handles *...> handles_;
    InvokerTupleType child_invokers_;
    Condition cancel_checker_ = {};
    std::array<size_t, sizeof...(Handles)> order_;
    Invoker *invoker_ = nullptr;
};

template <typename... Handles>
class WaitAllFinishHandle
    : public ParallelFinishHandle<WaitAllCancelCondition, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitAllCancelCondition, Handles...>;
    using ReturnType = std::tuple<typename Handles::ReturnType...>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <typename... Handles>
class WaitOneFinishHandle
    : public ParallelFinishHandle<WaitOneCancelCondition, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitOneCancelCondition, Handles...>;
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
            throw std::out_of_range("Index out of range in get_from_tuple_at_");
        }
    }
};

} // namespace condy
