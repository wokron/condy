#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/strategies.hpp"
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace condy {

class OpFinishHandle {
public:
    using ReturnType = int;

    void cancel() {
        auto ring = Context::current().get_ring();
        auto *sqe = Context::current().get_strategy()->get_sqe(ring);
        assert(sqe != nullptr);
        io_uring_prep_cancel(sqe, this, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    void set_on_finish(std::function<void(ReturnType)> on_finish) {
        on_finish_ = std::move(on_finish);
    }

    void finish(ReturnType r) {
        if (on_finish_) {
            std::move(on_finish_)(r);
        }
    }

private:
    std::function<void(ReturnType)> on_finish_ = nullptr;
};

template <typename Condition, typename Handle>
class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    template <typename Range> void init(Range &&handle_ptrs) {
        for (auto &handle : handle_ptrs) {
            auto no = handles_.size();
            auto on_finish = [this, no](ChildReturnType r) { finish_(no, r); };
            handle->set_on_finish(on_finish);
            handles_.push_back(handle);
        }
        order_.resize(handles_.size());
        results_.resize(handles_.size());
    }

    void cancel() {
        if (canceled_count_++ == 0) {
            for (auto &handle : handles_) {
                handle->cancel();
            }
        }
    }

    template <typename Func> void set_on_finish(Func &&on_finish) {
        on_finish_ = std::forward<Func>(on_finish);
    }

private:
    void finish_(size_t idx, ChildReturnType r) {
        size_t no = finished_count_++;
        order_[no] = idx;
        results_[idx] = r;
        if (no == handles_.size() - 1) {
            // All finished
            auto r = std::make_pair(std::move(order_), std::move(results_));
            std::move(on_finish_)(std::move(r));
            return;
        }

        if (cancel_checker_(idx, r) && canceled_count_++ == 0) {
            for (size_t i = 0; i < handles_.size(); i++) {
                if (i != idx) {
                    handles_[i]->cancel();
                }
            }
        }
    }

private:
    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::function<void(ReturnType)> on_finish_ = nullptr;
    std::vector<Handle *> handles_ = {};
    Condition cancel_checker_ = {};

    std::vector<size_t> order_ = {};
    std::vector<ChildReturnType> results_ = {};
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
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType = std::vector<ChildReturnType>;

    template <typename Func> void set_on_finish(Func &&on_finish) {
        auto wrapper =
            [on_finish = std::forward<Func>(on_finish)](
                std::pair<std::vector<size_t>, std::vector<ChildReturnType>>
                    r) { on_finish(std::move(r.second)); };
        Base::set_on_finish(std::move(wrapper));
    }
};

template <typename Handle>
class RangedWaitOneFinishHandle
    : public RangedParallelFinishHandle<WaitOneCancelCondition, Handle> {
public:
    using Base = RangedParallelFinishHandle<WaitOneCancelCondition, Handle>;
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType = std::pair<size_t, ChildReturnType>;

    template <typename Func> void set_on_finish(Func &&on_finish) {
        auto wrapper =
            [on_finish = std::forward<Func>(on_finish)](
                std::pair<std::vector<size_t>, std::vector<ChildReturnType>>
                    r) {
                auto &[order, results] = r;
                on_finish(
                    std::make_pair(order[0], std::move(results[order[0]])));
            };
        Base::set_on_finish(std::move(wrapper));
    }
};

template <typename Condition, typename... Handles> class ParallelFinishHandle {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Handles)>,
                                 std::tuple<typename Handles::ReturnType...>>;

    void init(Handles *...handles) {
        handles_ = std::make_tuple(handles...);
        foreach_call_set_on_finish_();
    }

    void cancel() {
        if (canceled_count_++ == 0) {
            constexpr size_t SkipIdx = std::numeric_limits<size_t>::max();
            foreach_call_cancel_<SkipIdx>();
        }
    }

    template <typename Func> void set_on_finish(Func &&on_finish) {
        on_finish_ = std::forward<Func>(on_finish);
    }

private:
    template <size_t I = 0> void foreach_call_set_on_finish_() {
        if constexpr (I < sizeof...(Handles)) {
            using Handle = std::remove_pointer_t<
                std::tuple_element_t<I, decltype(handles_)>>;
            auto handle = std::get<I>(handles_);
            handle->set_on_finish([this](typename Handle::ReturnType r) {
                this->template finish_<I, typename Handle::ReturnType>(
                    std::move(r));
            });
            foreach_call_set_on_finish_<I + 1>();
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

    template <size_t Idx, typename ChildReturnType>
    void finish_(ChildReturnType r) {
        size_t no = finished_count_++;
        order_[no] = Idx;
        std::get<Idx>(results_) = std::move(r);
        if (no == sizeof...(Handles) - 1) {
            // All finished
            auto r = std::make_pair(std::move(order_), std::move(results_));
            std::move(on_finish_)(std::move(r));
            return;
        }

        if (cancel_checker_(Idx, r) && canceled_count_++ == 0) {
            foreach_call_cancel_<Idx>();
        }
    }

private:
    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::function<void(ReturnType)> on_finish_ = nullptr;
    std::tuple<Handles *...> handles_;
    Condition cancel_checker_ = {};

    std::array<size_t, sizeof...(Handles)> order_ = {};
    std::tuple<typename Handles::ReturnType...> results_ = {};
};

template <typename... Handles>
class WaitAllFinishHandle
    : public ParallelFinishHandle<WaitAllCancelCondition, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitAllCancelCondition, Handles...>;
    using ReturnType = std::tuple<typename Handles::ReturnType...>;

    template <typename Func> void set_on_finish(Func &&on_finish) {
        auto wrapper =
            [on_finish = std::forward<Func>(on_finish)](
                std::pair<std::array<size_t, sizeof...(Handles)>,
                          std::tuple<typename Handles::ReturnType...>>
                    r) { on_finish(std::move(r.second)); };
        Base::set_on_finish(std::move(wrapper));
    }
};

template <typename... Handles>
class WaitOneFinishHandle
    : public ParallelFinishHandle<WaitOneCancelCondition, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitOneCancelCondition, Handles...>;
    using ReturnType = std::variant<typename Handles::ReturnType...>;

    template <typename Func> void set_on_finish(Func &&on_finish) {
        auto wrapper =
            [on_finish = std::forward<Func>(on_finish)](
                std::pair<std::array<size_t, sizeof...(Handles)>,
                          std::tuple<typename Handles::ReturnType...>>
                    r) {
                auto &[order, results] = r;
                size_t idx = order[0];
                on_finish(tuple_at_(std::move(results), idx));
            };
        Base::set_on_finish(std::move(wrapper));
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