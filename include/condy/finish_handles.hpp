#pragma once

#include "condy/context.hpp"
#include <array>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <variant>
#include <vector>

namespace condy {

class FinishHandleBase {
public:
    using OnFinishFunc = void (*)(void *self, size_t no);

    void set_on_finish(OnFinishFunc on_finish, void *self, size_t no) {
        on_finish_ = on_finish;
        data_ = self;
        no_ = no;
    }

    void set_on_finish(std::coroutine_handle<> handle,
                       bool self_delete = false) {
        data_ = handle.address();
        on_finish_ = nullptr;
        no_ = self_delete ? 1 : 0;
    }

    void invoke() {
        if (on_finish_) {
            on_finish_(data_, no_);
        } else {
            auto h = std::coroutine_handle<>::from_address(data_);
            if (no_) {
                delete this;
            }
            h.resume();
        }
    }

protected:
    OnFinishFunc on_finish_ = nullptr;
    void *data_ = nullptr;
    size_t no_ = 0;
};

class OpFinishHandle : public FinishHandleBase {
public:
    using ReturnType = int;

    void cancel() {
        auto ring = Context::current().get_ring();
        auto *sqe = Context::current().get_strategy()->get_sqe(ring);
        assert(sqe != nullptr);
        io_uring_prep_cancel(sqe, this, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    void invoke(int res) {
        res_ = std::move(res);
        FinishHandleBase::invoke();
    }

    int extract_result() { return res_; }

private:
    int res_;
};

template <typename Condition, typename Handle>
class RangedParallelFinishHandle : public FinishHandleBase {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    void init(std::vector<Handle *> handles) {
        handles_ = std::move(handles);
        for (size_t i = 0; i < handles_.size(); i++) {
            auto *handle = handles_[i];
            handle->set_on_finish(
                [](void *self, size_t no) {
                    auto *this_ptr = static_cast<
                        RangedParallelFinishHandle<Condition, Handle> *>(self);
                    this_ptr->finish_(no);
                },
                this, i);
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

private:
    void finish_(size_t idx) {
        size_t no = finished_count_++;
        auto &order = res_.first;
        auto &results = res_.second;
        order[no] = idx;
        results[idx] = handles_[idx]->extract_result();
        if (no == handles_.size() - 1) {
            // All finished
            invoke();
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
    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::vector<Handle *> handles_ = {};
    Condition cancel_checker_ = {};
    ReturnType res_;
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

template <typename Condition, typename... Handles>
class ParallelFinishHandle : public FinishHandleBase {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Handles)>,
                                 std::tuple<typename Handles::ReturnType...>>;

    template <typename... HandlePtr> void init(HandlePtr... handles) {
        handles_ = std::make_tuple(handles...);
        foreach_set_on_finish_();
    }

    void cancel() {
        if (canceled_count_++ == 0) {
            constexpr size_t SkipIdx = std::numeric_limits<size_t>::max();
            foreach_call_cancel_<SkipIdx>();
        }
    }

    ReturnType extract_result() { return std::move(res_); }

private:
    template <size_t I = 0> void foreach_set_on_finish_() {
        if constexpr (I < sizeof...(Handles)) {
            auto *handle = std::get<I>(handles_);
            handle->set_on_finish(
                [](void *self, size_t no) {
                    assert(no == I);
                    auto *this_ptr = static_cast<
                        ParallelFinishHandle<Condition, Handles...> *>(self);
                    this_ptr->template finish_<I>();
                },
                this, I);
            foreach_set_on_finish_<I + 1>();
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
            invoke();
            return;
        }

        if (cancel_checker_(Idx, std::get<Idx>(results)) &&
            canceled_count_++ == 0) {
            foreach_call_cancel_<Idx>();
        }
    }

private:
    size_t finished_count_ = 0;
    size_t canceled_count_ = 0;
    std::tuple<Handles *...> handles_;
    Condition cancel_checker_ = {};
    ReturnType res_;
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