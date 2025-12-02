#pragma once

#include "condy/buffers.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include <array>
#include <cstddef>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <limits>
#include <tuple>
#include <variant>
#include <vector>

namespace condy {

class Ring;

class OpFinishHandle : public InvokerAdapter<OpFinishHandle, WorkInvoker> {
public:
    using ReturnType = int;
    using MultiShotFunc = void (*)(void *);

    OpFinishHandle() { is_operation_ = true; }

    void cancel() {
        assert(ring_ != nullptr);
        io_uring_sqe *sqe = ring_->get_sqe();
        io_uring_prep_cancel(sqe, this, 0);
        io_uring_sqe_set_data(sqe, MagicData::IGNORE);
        io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
        ring_->maybe_submit();
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
    int res_;
    Invoker *invoker_ = nullptr;
    Ring *ring_ = nullptr;
    MultiShotFunc multishot_func_ = nullptr;
    int flags_;
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

template <typename HandleBase> class SelectBufferRecvMixin : public HandleBase {
public:
    using ReturnType = std::pair<int, std::vector<ProvidedBuffer>>;

    template <typename... Args>
    SelectBufferRecvMixin(detail::ProvidedBufferPoolImplPtr buffers_impl,
                          Args &&...args)
        : HandleBase(std::forward<Args>(args)...),
          buffers_impl_(std::move(buffers_impl)) {}

    ReturnType extract_result() {
        int res = this->res_;
        std::vector<ProvidedBuffer> entries;
        const size_t buf_size = buffers_impl_->buffer_size();
        if (this->flags_ & IORING_CQE_F_BUFFER) {
            assert(res >= 0);
            if (this->flags_ & IORING_CQE_F_BUF_MORE) {
                // Must be partial consumption
                assert(static_cast<size_t>(res) < buf_size);
                int bid = this->flags_ >> IORING_CQE_BUFFER_SHIFT;
                void *data =
                    buffers_impl_->get_buffer(static_cast<size_t>(bid));
                entries.emplace_back(nullptr, data, buf_size);
            } else {
                // One or more full buffers consumed
                const size_t buf_mask = buf_size - 1;
                size_t num_buffers = (res + buf_size - 1) / buf_size;
                int bid = this->flags_ >> IORING_CQE_BUFFER_SHIFT;
                for (size_t i = 0; i < num_buffers; i++) {
                    void *data =
                        buffers_impl_->get_buffer(static_cast<size_t>(bid));
                    // NOTE: No std::move here, since buffers_impl_ may be used
                    // multiple times (multishot)
                    entries.emplace_back(buffers_impl_, data, buf_size);
                    bid = (bid + 1) & buf_mask;
                }
            }
        }
        return std::make_pair(res, std::move(entries));
    }

private:
    detail::ProvidedBufferPoolImplPtr buffers_impl_;
};

template <typename HandleBase>
class SelectBufferNoBundleRecvMixin : public SelectBufferRecvMixin<HandleBase> {
public:
    using ReturnType = std::pair<int, ProvidedBuffer>;

    using Base = SelectBufferRecvMixin<HandleBase>;

    template <typename... Args>
    SelectBufferNoBundleRecvMixin(
        detail::ProvidedBufferPoolImplPtr buffers_impl, Args &&...args)
        : SelectBufferRecvMixin<HandleBase>(std::move(buffers_impl),
                                            std::forward<Args>(args)...) {}

    ReturnType extract_result() {
        auto result = Base::extract_result();
        auto &[res, entries] = result;
        if (entries.empty()) {
            assert(res < 0);
            return std::make_pair(res, ProvidedBuffer{});
        }
        assert(entries.size() == 1);
        return std::make_pair(res, std::move(entries[0]));
    }
};

using SelectBufferRecvOpFinishHandle = SelectBufferRecvMixin<OpFinishHandle>;

using SelectBufferNoBundleRecvOpFinishHandle =
    SelectBufferNoBundleRecvMixin<OpFinishHandle>;

template <typename MultiShotFunc>
using MultiShotSelectBufferRecvOpFinishHandle =
    MultiShotMixin<MultiShotFunc, SelectBufferRecvOpFinishHandle>;

template <typename MultiShotFunc>
using MultiShotSelectBufferNoBundleRecvOpFinishHandle =
    MultiShotMixin<MultiShotFunc, SelectBufferNoBundleRecvOpFinishHandle>;

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

template <typename HandleBase> class SelectBufferSendMixin : public HandleBase {
public:
    using ReturnType = int;

    template <typename... Args>
    SelectBufferSendMixin(detail::ProvidedBufferQueueImplPtr buffers_impl,
                          Args &&...args)
        : HandleBase(std::forward<Args>(args)...),
          buffers_impl_(std::move(buffers_impl)) {}

    ReturnType extract_result() {
        int res = this->res_;
        if (this->flags_ & IORING_CQE_F_BUFFER) {
            assert(res >= 0);
            int bid = this->flags_ >> IORING_CQE_BUFFER_SHIFT;
            if (!(this->flags_ & IORING_CQE_F_BUF_MORE)) {
                // Entire buffer has been sent, remove it from the queue
                // TODO: Add test for this
                buffers_impl_->remove_buffer(bid);
            }
        }
        return res;
    }

private:
    detail::ProvidedBufferQueueImplPtr buffers_impl_;
};

using SelectBufferSendOpFinishHandle = SelectBufferSendMixin<OpFinishHandle>;

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
