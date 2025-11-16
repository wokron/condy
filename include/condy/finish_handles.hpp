#pragma once

#include "condy/buffers.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include <array>
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

    OpFinishHandle() { is_operation_ = true; }

    void cancel() {
        assert(ring_ != nullptr);
        ring_->cancel_op(this);
    }

    void set_result(int res, int flags) {
        res_ = res;
        flags_ = flags;
    }

    void invoke() { (*invoker_)(); }

    ReturnType extract_result() { return res_; }

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

class TimerFinishHandle : public OpFinishHandle {};

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

template <typename HandleBase> class SelectBufferMixin : public HandleBase {
public:
    using ReturnType = std::pair<int, ProvidedBufferEntry>;

    template <typename... Args>
    SelectBufferMixin(detail::ProvidedBuffersImplPtr buffers_impl,
                      Args &&...args)
        : HandleBase(std::forward<Args>(args)...),
          buffers_impl_(std::move(buffers_impl)) {}

    ReturnType extract_result() {
        int res = this->res_;
        ProvidedBufferEntry entry;
        if (this->flags_ & IORING_CQE_F_BUFFER) {
            assert(res >= 0);
            int bid = this->flags_ >> IORING_CQE_BUFFER_SHIFT;
            void *data = buffers_impl_->get_buffer(static_cast<size_t>(bid));
            size_t size = buffers_impl_->buffer_size();
            detail::ProvidedBuffersImplPtr buffers_impl = nullptr;
            if (!(this->flags_ & IORING_CQE_F_BUF_MORE)) {
                // NOTE: No std::move here, since buffers_impl_ may be used
                // multiple times (multishot)
                buffers_impl = buffers_impl_; // The entire buffer is consumed
                // TODO: Add test case for this
            }
            entry = ProvidedBufferEntry(buffers_impl, data, size);
        }
        return std::make_pair(res, std::move(entry));
    }

private:
    detail::ProvidedBuffersImplPtr buffers_impl_;
};

using SelectBufferOpFinishHandle = SelectBufferMixin<OpFinishHandle>;

template <typename MultiShotFunc>
using MultiShotSelectBufferOpFinishHandle =
    MultiShotMixin<MultiShotFunc, SelectBufferOpFinishHandle>;

template <typename Func, typename HandleBase>
class ZeroCopyMixin : public HandleBase {
public:
    template <typename... Args>
    ZeroCopyMixin(Func func, Args &&...args)
        : HandleBase(std::forward<Args>(args)...), free_func_(std::move(func)) {
        this->multishot_func_ = &ZeroCopyMixin::invoke_multishot_;
        this->func_ =
            &ZeroCopyMixin::invoke_notify_; // Override the base invoke
    }

private:
    static void invoke_multishot_(void *data) {
        auto *self = static_cast<ZeroCopyMixin<Func, HandleBase> *>(data);
        (*self->invoker_)(); // Resume here
    }

    static void invoke_notify_(void *data) {
        auto *self = static_cast<ZeroCopyMixin<Func, HandleBase> *>(data);
        self->free_func_(self->res_);
        delete self; // TODO: Better way?
    }

private:
    Func free_func_;
};

template <typename FreeFunc>
using ZeroCopyOpFinishHandle = ZeroCopyMixin<FreeFunc, OpFinishHandle>;

template <bool Cancel, typename Handle> class RangedParallelFinishHandle {
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

constexpr bool WaitAll = false;

constexpr bool WaitOne = true;

template <typename Handle>
class RangedWaitAllFinishHandle
    : public RangedParallelFinishHandle<WaitAll, Handle> {
public:
    using Base = RangedParallelFinishHandle<WaitAll, Handle>;
    using ReturnType = std::vector<typename Handle::ReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <typename Handle>
class RangedWaitOneFinishHandle
    : public RangedParallelFinishHandle<WaitOne, Handle> {
public:
    using Base = RangedParallelFinishHandle<WaitOne, Handle>;
    using ChildReturnType = typename Handle::ReturnType;
    using ReturnType = std::pair<size_t, ChildReturnType>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return std::make_pair(order[0], std::move(results[order[0]]));
    }
};

template <bool Cancel, typename... Handles> class ParallelFinishHandle {
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

template <typename... Handles>
class WaitAllFinishHandle : public ParallelFinishHandle<WaitAll, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitAll, Handles...>;
    using ReturnType = std::tuple<typename Handles::ReturnType...>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        return std::move(r.second);
    }
};

template <typename... Handles>
class WaitOneFinishHandle : public ParallelFinishHandle<WaitOne, Handles...> {
public:
    using Base = ParallelFinishHandle<WaitOne, Handles...>;
    using ReturnType = std::variant<typename Handles::ReturnType...>;

    ReturnType extract_result() {
        auto r = Base::extract_result();
        auto &[order, results] = r;
        return tuple_at_(std::move(results), order[0]);
    }

private:
// TODO: investigate why clang optimization breaks this function
#pragma clang optimize off
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
#pragma clang optimize on
};

} // namespace condy
