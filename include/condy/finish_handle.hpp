#pragma once

#include <cstddef>
#include <functional>
#include <pthread.h>
#include <vector>

namespace condy {

template <typename Condition, typename Handle>
class RangedParallelFinishHandle {
public:
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType =
        std::pair<std::vector<size_t>, std::vector<ChildReturnType>>;

    template <typename Range> RangedParallelFinishHandle(Range &&handle_ptrs) {
        for (auto &handle : handle_ptrs) {
            auto no = handles_.size();
            auto on_finish = [this, no](ChildReturnType r) { finish_(no, r); };
            handle->set_on_finish(on_finish);
            handles_.push_back(handle);
        }
        order_.resize(handles_.size());
        results_.resize(handles_.size());
    }

public:
    void cancel() {
        for (auto &handle : handles_) {
            handle->cancel();
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

        if (cancel_checker_(idx, r)) {
            for (size_t i = 0; i < handles_.size(); i++) {
                if (i != idx) {
                    handles_[i]->cancel();
                }
            }
        }
    }

private:
    size_t finished_count_ = 0;
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

    using Base::Base;

public:
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

    using Base::Base;

public:
    template <typename Func> void set_on_finish(Func &&on_finish) {
        auto wrapper =
            [on_finish = std::forward<Func>(on_finish)](
                std::pair<std::vector<size_t>, std::vector<ChildReturnType>>
                    r) {
                auto &[order, results] = r;
                on_finish(order[0], std::move(results[order[0]]));
            };
        Base::set_on_finish(std::move(wrapper));
    }
};

} // namespace condy