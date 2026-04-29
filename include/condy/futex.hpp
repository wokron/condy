/**
 * @file futex.hpp
 * @brief User-space "futex" implementation for efficient synchronization
 * between coroutines.
 */

#pragma once

#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/type_traits.hpp"
#include <atomic>
#include <cerrno>
#include <optional>

namespace condy {

/**
 * @brief User-space "futex" implementation for efficient synchronization
 * between coroutines.
 * @details This class provides a user-space futex implementation that allows
 * coroutines to wait on a futex value and be efficiently notified when the
 * value changes. This class is different from condy::async_futex_wait(), the
 * latter one can be used together with thread-based synchronous wait.
 * @tparam T Type of the futex value.
 */
template <typename T> class Futex {
public:
    /**
     * @brief Construct a new Futex object
     * @param futex Reference to the atomic variable used as the futex.
     */
    Futex(std::atomic<T> &futex) : futex_(futex) {}

    ~Futex() { notify_all_(-EIDRM); }

    Futex(const Futex &) = delete;
    Futex &operator=(const Futex &) = delete;
    Futex(Futex &&) = delete;
    Futex &operator=(Futex &&) = delete;

public:
    struct [[nodiscard]] WaitSender;
    /**
     * @brief Wait if the futex value equals to the specified old value. The
     * awaiting coroutine will be suspended until a notify is received. If the
     * value of the futex is not equal to the old value, the awaiting coroutine
     * will not be suspended.
     * @param old The old value to compare with the futex value.
     * @return int32_t 0 if the wait operation is successful; -ECANCELED if the
     * wait operation is canceled while waiting; -EIDRM if the futex is
     * destroyed while waiting (usually indicates a lifetime management bug).
     */
    WaitSender wait(T old) noexcept { return {*this, old}; }

    /**
     * @brief Notify one awaiting coroutine, if any.
     * @note This function is thread-safe and can be called from any thread.
     */
    void notify_one() noexcept {
        WaitFinishHandleBase *handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = wait_awaiters_.pop_front();
        }
        if (handle) {
            handle->set_result(0);
            handle->schedule();
        }
    }

    /**
     * @brief Notify all awaiting coroutines.
     * @note This function is thread-safe and can be called from any thread.
     */
    void notify_all() noexcept { notify_all_(0); }

private:
    class WaitFinishHandleBase;
    template <typename Receiver> class WaitFinishHandle;

    bool cancel_wait_(WaitFinishHandleBase *handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return wait_awaiters_.remove(handle);
    }

    int32_t request_wait_(WaitFinishHandleBase *handle, T old) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto val = futex_.load(std::memory_order_relaxed);
        if (val != old) {
            return 0; // No need to wait
        }
        wait_awaiters_.push_back(handle);
        detail::Context::current().runtime()->pend_work();
        return -EAGAIN; // Need to wait
    }

private:
    void notify_all_(int32_t result) noexcept {
        HandleList handles;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handles = std::move(wait_awaiters_);
        }
        while (auto *handle = handles.pop_front()) {
            handle->set_result(result);
            handle->schedule();
        }
    }

private:
    using HandleList = IntrusiveDoubleList<WaitFinishHandleBase,
                                           &WaitFinishHandleBase::link_entry_>;

    mutable std::mutex mutex_;
    HandleList wait_awaiters_;
    std::atomic<T> &futex_;
};

template <typename T>
class Futex<T>::WaitFinishHandleBase : public WorkInvoker {
public:
    void schedule() noexcept {
        assert(runtime_ != nullptr);
        runtime_->schedule(this);
    }

    void set_result(int32_t result) noexcept { result_ = result; }

public:
    DoubleLinkEntry link_entry_;

protected:
    Runtime *runtime_ = nullptr;
    int32_t result_ = -ENOTRECOVERABLE; // Internal error if not set
};

template <typename T>
template <typename Receiver>
class Futex<T>::WaitFinishHandle
    : public InvokerAdapter<WaitFinishHandle<Receiver>, WaitFinishHandleBase> {
public:
    using Base = InvokerAdapter<WaitFinishHandle, WaitFinishHandleBase>;

    WaitFinishHandle(Futex &futex, Receiver receiver)
        : futex_(futex), receiver_(std::move(receiver)) {}

    void start(Runtime *runtime, T old) noexcept {
        this->runtime_ = runtime;
        int32_t r = futex_.request_wait_(this, old);
        if (r != -EAGAIN) {
            std::move(receiver_)(r);
            return;
        }

        auto stop_token = receiver_.get_stop_token();
        if (stop_token.stop_possible()) {
            stop_callback_.emplace(std::move(stop_token), Cancellation{this});
        }
    }

    void invoke() noexcept {
        stop_callback_.reset();
        assert(this->runtime_ != nullptr);
        this->runtime_->resume_work();
        std::move(receiver_)(this->result_);
    }

private:
    void cancel_() noexcept {
        if (futex_.cancel_wait_(this)) {
            // Successfully canceled
            this->result_ = -ECANCELED;
            assert(this->runtime_ != nullptr);
            this->runtime_->schedule(this);
        }
    }

    struct Cancellation {
        WaitFinishHandle *self;
        void operator()() noexcept { self->cancel_(); }
    };

    using StopCallbackType =
        stop_callback_t<stop_token_t<Receiver>, Cancellation>;

private:
    Futex &futex_;
    Receiver receiver_;
    std::optional<StopCallbackType> stop_callback_;
};

template <typename T> struct Futex<T>::WaitSender {
public:
    using ReturnType = int32_t;

    WaitSender(Futex &futex, T old) : futex_(futex), old_(old) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return OperationState<Receiver>(futex_, old_, std::move(receiver));
    }

private:
    template <typename Receiver>
    class OperationState : public WaitFinishHandle<Receiver> {
    public:
        using Base = WaitFinishHandle<Receiver>;
        OperationState(Futex &futex, T old, Receiver receiver)
            : Base(futex, std::move(receiver)), old_(old) {}

        void start(unsigned int /*flags*/) noexcept {
            auto *runtime = detail::Context::current().runtime();
            Base::start(runtime, old_);
        }

    private:
        T old_;
    };

private:
    Futex &futex_;
    T old_;
};

} // namespace condy