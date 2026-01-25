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
#include "condy/utils.hpp"
#include <coroutine>
#include <cstddef>
#include <optional>

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
     * @tparam U Type of the item to be pushed.
     * @param item The item to be pushed into the channel.
     * @return true If the item was successfully pushed.
     * @return false If the channel is full.
     * @throws std::logic_error If the channel is closed.
     */
    template <typename U> bool try_push(U &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_push_inner_(std::forward<U>(item));
    }

    /**
     * @brief Try to pop an item from the channel.
     * @return std::optional<T> The popped item if successful; std::nullopt if
     * the channel is empty or closed.
     */
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_pop_inner_();
    }

    template <typename U> void force_push(U &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (try_push_inner_(std::forward<U>(item))) [[likely]] {
            return;
        }
        auto *fake_handle = new PushFinishHandle(std::forward<U>(item));
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(fake_handle);
    }

    struct [[nodiscard]] PushAwaiter;
    /**
     * @brief Push an item into the channel, awaiting if necessary.
     * @tparam U Type of the item to be pushed.
     * @param item The item to be pushed into the channel.
     * @return PushAwaiter Awaiter object for the push operation.
     * @details This function attempts to push the given item into the channel.
     * If the channel is full, the coroutine will be suspended until space
     * becomes available. If the channel is closed, a std::logic_error will be
     * thrown.
     * @warning The item will be moved during the push operation. If the push
     * operation is cancelled, the moved item will be destroyed immediately and
     * will not be pushed into the channel.
     */
    template <typename U> PushAwaiter push(U &&item) {
        return {*this, std::forward<U>(item)};
    }

    struct [[nodiscard]] PopAwaiter;
    /**
     * @brief Pop an item from the channel, awaiting if necessary.
     * @return PopAwaiter Awaiter object for the pop operation.
     * @details This function attempts to pop an item from the channel. If the
     * channel is empty, the coroutine will be suspended until an item becomes
     * available. If the channel is closed, a default-constructed T will be
     * returned.
     */
    PopAwaiter pop() { return {*this}; }

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
        return size_;
    }

    /**
     * @brief Check if the channel is empty.
     * @warning This function may not be accurate in multithreaded scenarios.
     */
    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
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
     * no more items can be pushed into the channel. All pending and future pop
     * operations will return default-constructed T values. All pending push
     * operations will throw std::logic_error.
     * @note This function is idempotent.
     */
    void push_close() {
        std::lock_guard<std::mutex> lock(mutex_);
        push_close_inner_();
    }

private:
    class PushFinishHandle;
    class PopFinishHandle;

    bool request_push_(PushFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (try_push_inner_(std::move(finish_handle->get_item()))) {
            return true;
        }
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(finish_handle);
        detail::Context::current().runtime()->pend_work();
        return false;
    }

    bool cancel_push_(PushFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return push_awaiters_.remove(finish_handle);
    }

    std::optional<T> request_pop_(PopFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = try_pop_inner_();
        if (result.has_value()) {
            return result;
        }
        assert(push_awaiters_.empty());
        pop_awaiters_.push_back(finish_handle);
        detail::Context::current().runtime()->pend_work();
        return std::nullopt;
    }

    bool cancel_pop_(PopFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_awaiters_.remove(finish_handle);
    }

private:
    template <typename U> bool try_push_inner_(U &&item) {
        if (closed_) [[unlikely]] {
            throw std::logic_error("Push to closed channel");
        }
        if (!pop_awaiters_.empty()) {
            assert(empty_inner_());
            auto *pop_handle = pop_awaiters_.pop_front();
            pop_handle->set_result(std::forward<U>(item));
            pop_handle->schedule();
            return true;
        }
        if (!full_inner_()) {
            push_inner_(std::forward<U>(item));
            return true;
        }
        return false;
    }

    std::optional<T> try_pop_inner_() {
        if (!push_awaiters_.empty()) {
            assert(full_inner_());
            auto *push_handle = push_awaiters_.pop_front();
            T item = std::move(push_handle->get_item());
            push_handle->schedule();
            if (no_buffer_()) {
                return item;
            } else {
                T result = pop_inner_();
                push_inner_(std::move(item));
                return result;
            }
        }
        if (!empty_inner_()) {
            T result = pop_inner_();
            return result;
        }
        if (closed_) [[unlikely]] {
            // Default indicates closed channel
            T return_value = {};
            return return_value;
        }
        return std::nullopt;
    }

    template <typename U> void push_inner_(U &&item) {
        assert(!full_inner_());
        auto mask = buffer_.capacity() - 1;
        buffer_[tail_ & mask].construct(std::forward<U>(item));
        tail_++;
        size_++;
    }

    T pop_inner_() {
        assert(!empty_inner_());
        auto mask = buffer_.capacity() - 1;
        T item = std::move(buffer_[head_ & mask].get());
        buffer_[head_ & mask].destroy();
        head_++;
        size_--;
        return item;
    }

    bool no_buffer_() const noexcept { return buffer_.capacity() == 0; }

    bool empty_inner_() const noexcept {
        if (no_buffer_()) {
            return true;
        }
        return size_ == 0;
    }

    bool full_inner_() const noexcept {
        if (no_buffer_()) {
            return true;
        }
        return size_ == buffer_.capacity();
    }

    void push_close_inner_() {
        if (closed_) {
            return;
        }
        closed_ = true;
        // Cancel all pending pop awaiters
        PopFinishHandle *pop_handle = nullptr;
        while ((pop_handle = pop_awaiters_.pop_front()) != nullptr) {
            assert(empty_inner_());
            pop_handle->schedule();
        }
        // Throw exception to all pending push awaiters
        PushFinishHandle *push_handle = nullptr;
        while ((push_handle = push_awaiters_.pop_front()) != nullptr) {
            push_handle->enable_throw();
            push_handle->schedule();
        }
    }

    void destruct_all_() {
        while (!empty_inner_()) {
            pop_inner_();
        }
        assert(size_ == 0);
        assert(head_ == tail_);
    }

private:
    template <typename Handle>
    using HandleList = IntrusiveDoubleList<Handle, &Handle::link_entry_>;

    mutable std::mutex mutex_;
    HandleList<PushFinishHandle> push_awaiters_;
    HandleList<PopFinishHandle> pop_awaiters_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
    SmallArray<RawStorage<T>, N> buffer_;
    bool closed_ = false;
};

template <typename T, size_t N>
class Channel<T, N>::PushFinishHandle
    : public InvokerAdapter<PushFinishHandle, WorkInvoker> {
public:
    using ReturnType = bool;

    PushFinishHandle(T item) : item_(std::move(item)) {}

    void cancel() {
        if (channel_->cancel_push_(this)) {
            // Successfully canceled
            canceled_ = true;
            runtime_->resume_work();
            runtime_->schedule(this);
        }
    }

    ReturnType extract_result() {
        if (should_throw_) [[unlikely]] {
            throw std::logic_error("Push to closed channel");
        }
        bool success = !canceled_;
        return success;
    }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void invoke() {
        if (need_resume_) {
            runtime_->resume_work();
        }
        (*invoker_)();
    }

public:
    void init(Channel *channel, Runtime *runtime) {
        channel_ = channel;
        runtime_ = runtime;
    }

    T &get_item() { return item_; }

    void schedule() {
        if (runtime_ == nullptr) [[unlikely]] {
            // Fake handle, no need to schedule
            delete this;
        } else {
            need_resume_ = true;
            runtime_->schedule(this);
        }
    }

    void enable_throw() { should_throw_ = true; }

public:
    DoubleLinkEntry link_entry_;

private:
    Invoker *invoker_ = nullptr;
    Channel *channel_ = nullptr;
    Runtime *runtime_ = nullptr;
    T item_;
    bool need_resume_ = false;
    bool should_throw_ = false;
    bool canceled_ = false;
};

template <typename T, size_t N>
class Channel<T, N>::PopFinishHandle
    : public InvokerAdapter<PopFinishHandle, WorkInvoker> {
public:
    using ReturnType = T;

    void cancel() {
        if (channel_->cancel_pop_(this)) {
            // Successfully canceled
            runtime_->resume_work();
            runtime_->schedule(this);
        }
    }

    ReturnType extract_result() { return std::move(result_); }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void invoke() {
        if (need_resume_) {
            runtime_->resume_work();
        }
        (*invoker_)();
    }

public:
    void init(Channel *channel, Runtime *runtime) {
        channel_ = channel;
        runtime_ = runtime;
    }

    void set_result(T result) { result_ = std::move(result); }

    void schedule() {
        assert(runtime_ != nullptr);
        need_resume_ = true;
        runtime_->schedule(this);
    }

public:
    DoubleLinkEntry link_entry_;

private:
    Invoker *invoker_ = nullptr;
    Channel *channel_ = nullptr;
    Runtime *runtime_ = nullptr;
    T result_ = {};
    bool need_resume_ = false;
};

/**
 * @brief Awaiter for pushing an item into the channel.
 * @return True if the push operation was successful after awaiting; false if
 * the operation was cancelled.
 * @throws std::logic_error If the channel is closed.
 */
template <typename T, size_t N> struct Channel<T, N>::PushAwaiter {
public:
    using HandleType = PushFinishHandle;

    PushAwaiter(Channel &channel, T item)
        : channel_(channel), finish_handle_(std::move(item)) {}
    PushAwaiter(PushAwaiter &&) = default;

    PushAwaiter(const PushAwaiter &) = delete;
    PushAwaiter &operator=(const PushAwaiter &) = delete;
    PushAwaiter &operator=(PushAwaiter &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) {
        auto *runtime = detail::Context::current().runtime();
        finish_handle_.init(&channel_, runtime);
        bool ok = channel_.request_push_(&finish_handle_);
        if (ok) {
            runtime->schedule(&finish_handle_);
        }
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        finish_handle_.init(&channel_, detail::Context::current().runtime());
        bool ok = channel_.request_push_(&finish_handle_);
        bool do_suspend = !ok;
        return do_suspend;
    }

    auto await_resume() { return finish_handle_.extract_result(); }

private:
    Channel &channel_;
    PushFinishHandle finish_handle_;
};

/**
 * @brief Awaiter for popping an item from the channel.
 * @return The popped item after awaiting. If the channel is closed, a
 * default-constructed T will be returned.
 */
template <typename T, size_t N> struct Channel<T, N>::PopAwaiter {
public:
    using HandleType = PopFinishHandle;

    PopAwaiter(Channel &channel) : channel_(channel) {}
    PopAwaiter(PopAwaiter &&) = default;

    PopAwaiter(const PopAwaiter &) = delete;
    PopAwaiter &operator=(const PopAwaiter &) = delete;
    PopAwaiter &operator=(PopAwaiter &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) {
        auto *runtime = detail::Context::current().runtime();
        finish_handle_.init(&channel_, runtime);
        auto item = channel_.request_pop_(&finish_handle_);
        if (item.has_value()) {
            finish_handle_.set_result(std::move(item.value()));
            runtime->schedule(&finish_handle_);
        }
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        finish_handle_.init(&channel_, detail::Context::current().runtime());
        auto item = channel_.request_pop_(&finish_handle_);
        if (item.has_value()) {
            finish_handle_.set_result(std::move(item.value()));
            return false; // Do not suspend
        }
        return true; // Suspend
    }

    auto await_resume() { return finish_handle_.extract_result(); }

private:
    Channel &channel_;
    PopFinishHandle finish_handle_;
};

} // namespace condy