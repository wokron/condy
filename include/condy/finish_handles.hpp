#pragma once

#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

namespace condy {

class OpFinishHandle : public InvokerAdapter<OpFinishHandle, WorkInvoker> {
public:
    DoubleLinkEntry link_; // For outstanding ops list

public:
    using ReturnType = int;

    void cancel();

    void set_result(int res) { res_ = res; }

    void operator()() { (*invoker_)(); }

    int extract_result() { return res_; }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    bool is_stealable() const { return stealable_; }

    void set_stealable(bool stealable) { stealable_ = stealable; }

private:
    bool stealable_ = true;
    int res_;
    Invoker *invoker_ = nullptr;
};

template <typename Condition, typename Handle>
class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;
    static constexpr bool is_stealable = Condition::is_stealable;

    void init(std::vector<Handle *> handles) {
        handles_ = std::move(handles);
        invokers_.resize(handles_.size());
        for (size_t i = 0; i < handles_.size(); i++) {
            auto *handle = handles_[i];
            auto &invoker = invokers_[i];
            invoker.self_ = this;
            invoker.no_ = i;
            handle->set_invoker(&invoker);
        }
        res_.first.resize(handles_.size());
        res_.second.resize(handles_.size());
    }

    void cancel() {
        if (canceled_count_++ == 0) {
            for (auto &handle : handles_) {
                handle->cancel();
            }
        }
    }

    ReturnType extract_result() { return std::move(res_); }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

private:
    void finish_(size_t idx) {
        size_t no = finished_count_++;
        auto &order = res_.first;
        auto &results = res_.second;
        order[no] = idx;
        results[idx] = handles_[idx]->extract_result();
        if (no == handles_.size() - 1) {
            // All finished
            (*invoker_)();
            return;
        }

        if (cancel_checker_(idx, results[idx]) && canceled_count_++ == 0) {
            for (size_t i = 0; i < handles_.size(); i++) {
                if (i != idx) {
                    handles_[i]->cancel();
                }
            }
        }
    }

private:
    struct FinishInvoker : public InvokerAdapter<FinishInvoker> {
        void operator()() { self_->finish_(no_); }
        RangedParallelFinishHandle *self_;
        size_t no_;
    };

    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::vector<Handle *> handles_ = {};
    std::vector<FinishInvoker> invokers_;
    Condition cancel_checker_ = {};
    ReturnType res_;
    Invoker *invoker_ = nullptr;
};

struct WaitAllCancelCondition {
    template <typename... Args> bool operator()(Args &&...args) const {
        return false;
    }

    static constexpr bool is_stealable = true;
};

struct WaitOneCancelCondition {
    template <typename... Args> bool operator()(Args &&...args) const {
        return true;
    }

    static constexpr bool is_stealable = false;
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
    static constexpr bool is_stealable = Condition::is_stealable;

    template <typename... HandlePtr> void init(HandlePtr... handles) {
        handles_ = std::make_tuple(handles...);
        foreach_set_invoker_();
    }

    void cancel() {
        if (canceled_count_++ == 0) {
            constexpr size_t SkipIdx = std::numeric_limits<size_t>::max();
            foreach_call_cancel_<SkipIdx>();
        }
    }

    ReturnType extract_result() { return std::move(res_); }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

private:
    template <size_t I = 0> void foreach_set_invoker_() {
        if constexpr (I < sizeof...(Handles)) {
            auto *handle = std::get<I>(handles_);
            auto &invoker = std::get<I>(invokers_);
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
        auto &[order, results] = res_;
        order[no] = Idx;
        std::get<Idx>(results) = std::get<Idx>(handles_)->extract_result();
        if (no == sizeof...(Handles) - 1) {
            // All finished
            (*invoker_)();
            return;
        }

        if (cancel_checker_(Idx, std::get<Idx>(results)) &&
            canceled_count_++ == 0) {
            foreach_call_cancel_<Idx>();
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

    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::tuple<Handles *...> handles_;
    InvokerTupleType invokers_;
    Condition cancel_checker_ = {};
    ReturnType res_;
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

#include "condy/finish_handles.inl"