/**
 * @file channel.hpp
 * @brief Thread-safe channel type for communication and synchronization.
 * @details This file defines a thread-safe channel type, which can be used for
 * communication and synchronization across different Runtimes.
 */

#pragma once

#include "condy/context.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/type_traits.hpp"
#include "condy/utils.hpp"
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

namespace condy {

/**
 * @brief Thread-safe bounded channel for communication and synchronization.
 * @tparam T Type of the items transmitted through the channel.
 * @tparam N When the capacity is less than or equal to N, the channel uses
 * stack storage for buffering; otherwise, it uses heap storage.
 * @details This class provides a thread-safe channel for communication and
 * synchronization for both intra-Runtime and inter-Runtime scenarios. It
 * supports both buffered and unbuffered modes, as well as push and pop
 * operations that can be awaited in coroutines. The internal implementation
 * utilizes msg_ring operations of io_uring for efficient cross-runtime
 * notifications.
 */
template <typename T, size_t N = 2> class Channel {
public:
    /**
     * @brief Construct a new Channel object
     * @param capacity Capacity of the channel. If capacity is zero, the channel
     * operates in unbuffered mode.
     */
    Channel(size_t capacity)
        : buffer_(capacity ? std::bit_ceil(capacity) : 0) {}
    ~Channel() {
        std::lock_guard<std::mutex> lock(mutex_);
        push_close_inner_();
        destruct_all_();
    }

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    Channel(Channel &&) = delete;
    Channel &operator=(Channel &&) = delete;

public:
    /**
     * @brief Try to push an item into the channel.
     * @param item The item to be pushed into the channel.
     * @return int32_t 0 if the item was successfully pushed; -EPIPE if the
     * channel is closed; -EAGAIN if the channel is full.
     */
    template <typename U>
        requires std::is_same_v<std::decay_t<U>, T>
    int32_t try_push(U &&item) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return -EPIPE;
        }
        if (try_push_inner_(std::forward<U>(item))) {
            return 0;
        }
        return -EAGAIN;
    }

    /**
     * @brief Try to pop an item from the channel.
     * @return std::pair<int32_t, T> 0 and the popped item if successful;
     * -EPIPE if the channel is closed and no more items can be popped;
     * -EAGAIN if the channel is empty.
     */
    std::pair<int32_t, T> try_pop() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto item = try_pop_inner_();
        if (item.has_value()) {
            return {0, std::move(item.value())};
        } else if (closed_) {
            return {-EPIPE, T()};
        } else {
            return {-EAGAIN, T()};
        }
    }

    void force_push(T item) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) [[unlikely]] {
            panic_on("Push to closed channel");
        }
        if (try_push_inner_(std::move(item))) [[likely]] {
            return;
        }
        // TODO: re-enable this
        // // This is safe because if try_push_inner_ returns false, the item
        // has
        // // not been moved into the channel.
        // // NOLINTBEGIN(bugprone-use-after-move)
        // auto *fake_handle =
        //     new (std::nothrow) PushFinishHandleBase(std::move(item));
        // // NOLINTEND(bugprone-use-after-move)
        // if (!fake_handle) {
        //     panic_on("Allocation failed for PushFinishHandle");
        // }
        // assert(pop_awaiters_.empty());
        // push_awaiters_.push_back(fake_handle);
    }

    class [[nodiscard]] PushSender;
    using PushAwaiter = PushSender;
    class [[nodiscard]] CopyPushSender;
    /**
     * @brief Push an item into the channel, awaiting if necessary.
     * @param item The item to be pushed into the channel.
     * @return int32_t 0 if the item was successfully pushed; -EPIPE if the
     * channel is closed; -ECANCELED if the operation was cancelled while
     * waiting.
     * @warning The item will be moved during the push operation. If the push
     * operation is cancelled, the moved item will be destroyed immediately and
     * will not be pushed into the channel.
     */
    PushAwaiter push(T &&item) noexcept { return {*this, std::move(item)}; }
    CopyPushSender push(const T &item) noexcept { return {*this, item}; }

    class [[nodiscard]] PopSender;
    using PopAwaiter = PopSender;
    /**
     * @brief Pop an item from the channel, awaiting if necessary.
     * @return std::pair<int32_t, T> 0 and the popped item if successful; -EPIPE
     * if the channel is closed and no more items can be popped; -ECANCELED if
     * the operation was cancelled while waiting.
     */
    PopAwaiter pop() noexcept { return {*this}; }

    /**
     * @brief Get the capacity of the channel.
     */
    size_t capacity() const noexcept { return buffer_.capacity(); }

    /**
     * @brief Get the current size of the channel.
     * @warning This function may not be accurate in multithreaded scenarios.
     */
    size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_inner_();
    }

    /**
     * @brief Check if the channel is empty.
     * @warning This function may not be accurate in multithreaded scenarios.
     */
    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return empty_inner_();
    }

    /**
     * @brief Check if the channel is closed.
     * @warning This function may not be accurate in multithreaded scenarios.
     */
    bool is_closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /**
     * @brief Close the channel.
     * @details This function closes the channel. After the channel is closed,
     * no more items can be pushed into the channel. All pending and future push
     * operations will fail with -EPIPE. All pending pop operations will fail
     * with -EPIPE if there are no more items to pop.
     * @note This function is idempotent.
     */
    void push_close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        push_close_inner_();
    }

private:
    class PushFinishHandleBase;
    template <typename Receiver> class PushFinishHandle;

    class PopFinishHandleBase;
    template <typename Receiver> class PopFinishHandle;

    int32_t request_push_(PushFinishHandleBase *finish_handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return -EPIPE;
        }
        if (try_push_inner_(std::move(finish_handle->get_item()))) {
            return 0;
        }
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(finish_handle);
        detail::Context::current().runtime()->pend_work();
        return -EAGAIN;
    }

    bool cancel_push_(PushFinishHandleBase *finish_handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return push_awaiters_.remove(finish_handle);
    }

    std::pair<int32_t, T>
    request_pop_(PopFinishHandleBase *finish_handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = try_pop_inner_();
        if (result.has_value()) {
            return {0, std::move(result.value())};
        }
        assert(push_awaiters_.empty());
        if (closed_) {
            return {-EPIPE, T()};
        }
        pop_awaiters_.push_back(finish_handle);
        detail::Context::current().runtime()->pend_work();
        return {-EAGAIN, T()};
    }

    bool cancel_pop_(PopFinishHandleBase *finish_handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_awaiters_.remove(finish_handle);
    }

private:
    template <typename U>
        requires std::is_same_v<std::decay_t<U>, T>
    bool try_push_inner_(U &&item) noexcept {
        if (!pop_awaiters_.empty()) {
            assert(empty_inner_());
            auto *pop_handle = pop_awaiters_.pop_front();
            pop_handle->set_result({0, std::forward<U>(item)});
            pop_handle->schedule();
            return true;
        }
        if (!full_inner_()) {
            push_inner_(std::forward<U>(item));
            return true;
        }
        return false;
    }

    std::optional<T> try_pop_inner_() noexcept {
        if (!push_awaiters_.empty()) {
            assert(full_inner_());
            auto *push_handle = push_awaiters_.pop_front();
            T item = std::move(push_handle->get_item());
            push_handle->set_result(0);
            push_handle->schedule();
            return pop_and_push_(std::move(item));
        }
        if (!empty_inner_()) {
            T result = pop_inner_();
            return result;
        }
        return std::nullopt;
    }

    T pop_and_push_(T item) noexcept {
        if (no_buffer_()) {
            return item;
        } else {
            T result = pop_inner_();
            push_inner_(std::move(item));
            return result;
        }
    }

    template <typename U>
        requires std::is_same_v<std::decay_t<U>, T>
    void push_inner_(U &&item) noexcept {
        assert(!full_inner_());
        auto mask = buffer_.capacity() - 1;
        buffer_[tail_ & mask].construct(std::forward<U>(item));
        tail_++;
    }

    T pop_inner_() noexcept {
        assert(!empty_inner_());
        auto mask = buffer_.capacity() - 1;
        T item = std::move(buffer_[head_ & mask].get());
        buffer_[head_ & mask].destroy();
        head_++;
        return item;
    }

    bool no_buffer_() const noexcept { return buffer_.capacity() == 0; }

    bool empty_inner_() const noexcept { return size_inner_() == 0; }

    bool full_inner_() const noexcept {
        return size_inner_() == buffer_.capacity();
    }

    void push_close_inner_() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        // Cancel all pending pop awaiters
        PopFinishHandleBase *pop_handle = nullptr;
        while ((pop_handle = pop_awaiters_.pop_front()) != nullptr) {
            assert(empty_inner_());
            pop_handle->set_result({-EPIPE, T()});
            pop_handle->schedule();
        }
        // Cancel all pending push awaiters
        PushFinishHandleBase *push_handle = nullptr;
        while ((push_handle = push_awaiters_.pop_front()) != nullptr) {
            assert(full_inner_());
            push_handle->set_result(-EPIPE);
            push_handle->schedule();
        }
    }

    void destruct_all_() noexcept {
        while (!empty_inner_()) {
            pop_inner_();
        }
        assert(head_ == tail_);
    }

    size_t size_inner_() const noexcept { return tail_ - head_; }

private:
    template <typename Handle>
    using HandleList = IntrusiveDoubleList<Handle, &Handle::link_entry_>;

    mutable std::mutex mutex_;
    HandleList<PushFinishHandleBase> push_awaiters_;
    HandleList<PopFinishHandleBase> pop_awaiters_;
    size_t head_ = 0;
    size_t tail_ = 0;
    SmallArray<RawStorage<T>, N> buffer_;
    bool closed_ = false;
};

template <typename T, size_t N>
class Channel<T, N>::PushFinishHandleBase : public WorkInvoker {
public:
    PushFinishHandleBase(T &item) : item_(item) {}

    void schedule() noexcept {
        if (runtime_ == nullptr) [[unlikely]] {
            // Fake handle, no need to schedule
            delete this;
        } else {
            runtime_->schedule(this);
        }
    }

    T &get_item() noexcept { return item_; }

    void set_result(int32_t result) noexcept { result_ = result; }

public:
    DoubleLinkEntry link_entry_;

public:
    Runtime *runtime_ = nullptr;
    T &item_;
    int32_t result_ = -ENOTRECOVERABLE; // Internal error if not set
};

template <typename T, size_t N>
template <typename Receiver>
class Channel<T, N>::PushFinishHandle
    : public InvokerAdapter<PushFinishHandle<Receiver>, PushFinishHandleBase> {
public:
    using Base =
        InvokerAdapter<PushFinishHandle<Receiver>, PushFinishHandleBase>;

    PushFinishHandle(Channel &channel, T &item, Receiver receiver)
        : Base(item), channel_(channel), receiver_(std::move(receiver)) {}

    void start(Runtime *runtime) noexcept {
        this->runtime_ = runtime;
        int32_t r = channel_.request_push_(this);
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
        if (channel_.cancel_push_(this)) {
            // Successfully canceled
            assert(this->result_ == -ENOTRECOVERABLE);
            this->result_ = -ECANCELED;
            assert(this->runtime_ != nullptr);
            this->runtime_->schedule(this);
        }
    }

    struct Cancellation {
        PushFinishHandle *self;
        void operator()() noexcept { self->cancel_(); }
    };

    using StopCallbackType =
        stop_callback_t<stop_token_t<Receiver>, Cancellation>;

private:
    Channel &channel_;
    Receiver receiver_;
    std::optional<StopCallbackType> stop_callback_;
};

template <typename T, size_t N>
class Channel<T, N>::PopFinishHandleBase : public WorkInvoker {
public:
    void schedule() noexcept {
        assert(runtime_ != nullptr);
        runtime_->schedule(this);
    }

    void set_result(std::pair<int32_t, T> result) noexcept {
        result_ = std::move(result);
    }

public:
    DoubleLinkEntry link_entry_;

protected:
    Runtime *runtime_ = nullptr;
    // Internal error if not set
    std::pair<int32_t, T> result_ = {-ENOTRECOVERABLE, T()};
};

template <typename T, size_t N>
template <typename Receiver>
class Channel<T, N>::PopFinishHandle
    : public InvokerAdapter<PopFinishHandle<Receiver>, PopFinishHandleBase> {
public:
    PopFinishHandle(Channel &channel, Receiver receiver)
        : channel_(channel), receiver_(std::move(receiver)) {}

    void start(Runtime *runtime) noexcept {
        this->runtime_ = runtime;
        auto item = channel_.request_pop_(this);
        auto r = item.first;
        if (r != -EAGAIN) {
            std::move(receiver_)(std::move(item));
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
        std::move(receiver_)(std::move(this->result_));
    }

private:
    void cancel_() noexcept {
        if (channel_.cancel_pop_(this)) {
            // Successfully canceled
            assert(this->result_.first == -ENOTRECOVERABLE);
            this->result_.first = -ECANCELED;
            assert(this->runtime_ != nullptr);
            this->runtime_->schedule(this);
        }
    }

    struct Cancellation {
        PopFinishHandle *self;
        void operator()() noexcept { self->cancel_(); }
    };

    using StopCallbackType =
        stop_callback_t<stop_token_t<Receiver>, Cancellation>;

private:
    Channel &channel_;
    Receiver receiver_;
    std::optional<StopCallbackType> stop_callback_;
};

template <typename T, size_t N> class Channel<T, N>::PushSender {
public:
    using ReturnType = int32_t;

    PushSender(Channel &channel, T &&item)
        : channel_(channel), item_(std::move(item)) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return OperationState<Receiver>(channel_, std::move(item_),
                                        std::move(receiver));
    }

private:
    template <typename Receiver>
    class OperationState
        : public Channel<T, N>::template PushFinishHandle<Receiver> {
    public:
        using Base =
            typename Channel<T, N>::template PushFinishHandle<Receiver>;
        OperationState(Channel &channel, T &&item, Receiver receiver)
            : Base(channel, item, std::move(receiver)) {}

        void start(unsigned int /*flags*/) noexcept {
            auto *runtime = detail::Context::current().runtime();
            Base::start(runtime);
        }
    };

    Channel &channel_;
    T &&item_;
};

template <typename T, size_t N> class Channel<T, N>::CopyPushSender {
public:
    using ReturnType = int32_t;

    CopyPushSender(Channel &channel, const T &item)
        : channel_(channel), item_(item) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return OperationState<Receiver>(channel_, item_, std::move(receiver));
    }

private:
    template <typename Receiver>
    class OperationState
        : public Channel<T, N>::template PushFinishHandle<Receiver> {
    public:
        using Base =
            typename Channel<T, N>::template PushFinishHandle<Receiver>;
        OperationState(Channel &channel, const T &item, Receiver receiver)
            : Base(channel, item_copy_, std::move(receiver)), item_copy_(item) {
        }

        void start(unsigned int /*flags*/) noexcept {
            auto *runtime = detail::Context::current().runtime();
            Base::start(runtime);
        }

    private:
        T item_copy_;
    };

    Channel &channel_;
    const T &item_;
};

template <typename T, size_t N> class Channel<T, N>::PopSender {
public:
    using ReturnType = std::pair<int32_t, T>;

    PopSender(Channel &channel) : channel_(channel) {}

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return OperationState<Receiver>(channel_, std::move(receiver));
    }

private:
    template <typename Receiver>
    class OperationState
        : public Channel<T, N>::template PopFinishHandle<Receiver> {
    public:
        using Base = typename Channel<T, N>::template PopFinishHandle<Receiver>;
        using Base::Base;

        void start(unsigned int /*flags*/) noexcept {
            auto *runtime = detail::Context::current().runtime();
            Base::start(runtime);
        }
    };

    Channel &channel_;
};

} // namespace condy
