#pragma once

#include "condy/context.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/utils.hpp"
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <variant>

namespace condy {

template <typename T> class Channel;

template <typename T>
class ChannelPushFinishHandle
    : public InvokerAdapter<ChannelPushFinishHandle<T>, WorkInvoker> {
public:
    using ReturnType = std::monostate;

    void cancel();

    ReturnType extract_result() { return {}; }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void invoke() {
        Context::current().runtime()->resume_work();
        (*invoker_)();
    }

    void set_channel(Channel<T> *channel) { channel_ = channel; }

    void set_item(T item) { item_ = std::move(item); }

    T &get_item() { return item_; }

    void set_runtime(Runtime *runtime) { runtime_ = runtime; }

    Runtime *get_runtime() const { return runtime_; }

public:
    DoubleLinkEntry link_entry_;

private:
    T item_;
    Invoker *invoker_ = nullptr;
    Channel<T> *channel_ = nullptr;
    Runtime *runtime_ = nullptr;
};

template <typename T>
class ChannelPopFinishHandle
    : public InvokerAdapter<ChannelPopFinishHandle<T>, WorkInvoker> {
public:
    using ReturnType = T;

    void cancel();

    ReturnType extract_result() { return std::move(result_); }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void invoke() {
        Context::current().runtime()->resume_work();
        (*invoker_)();
    }

    void set_channel(Channel<T> *channel) { channel_ = channel; }

    void set_result(T result) { result_ = std::move(result); }

    void set_runtime(Runtime *runtime) { runtime_ = runtime; }

    Runtime *get_runtime() const { return runtime_; }

public:
    DoubleLinkEntry link_entry_;

private:
    T result_;
    Invoker *invoker_ = nullptr;
    Channel<T> *channel_ = nullptr;
    Runtime *runtime_ = nullptr;
};

template <typename T> class ChannelPopFinishHandle;

// TODO: Need to refactor
template <typename T> class Channel {
public:
    Channel(size_t capacity)
        : buffer_(std::make_unique<Uninitialized<T>[]>(capacity + 1)),
          actual_capacity_(capacity + 1) {}
    ~Channel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!push_awaiters_.empty() || !pop_awaiters_.empty()) {
            // There are still awaiters, this is likely a programming error
            panic_on("Channel destroyed with pending awaiters");
        }
    }

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    Channel(Channel &&) = delete;
    Channel &operator=(Channel &&) = delete;

public:
    template <typename U> bool try_push(U &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_push_inner_(std::forward<U>(item));
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_pop_inner_();
    }

    template <typename U> void force_push(U &&item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (try_push_inner_(std::forward<U>(item))) [[likely]] {
            return;
        }
        ChannelPushFinishHandle<T> *fake_handle =
            new ChannelPushFinishHandle<T>();
        fake_handle->set_item(std::forward<U>(item));
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(fake_handle);
    }

    struct [[nodiscard]] ChannelPushAwaiter;
    template <typename U> ChannelPushAwaiter push(U &&item) {
        return {*this, std::forward<U>(item)};
    }

    struct [[nodiscard]] ChannelPopAwaiter;
    ChannelPopAwaiter pop() { return {*this}; }

    size_t capacity() const noexcept { return actual_capacity_ - 1; }

    size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tail_ >= head_) {
            return tail_ - head_;
        } else {
            // actual_capacity_ - (head_ - tail_)
            return actual_capacity_ - head_ + tail_;
        }
    }

    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return empty_inner_();
    }

public:
    bool request_push(T item, ChannelPushFinishHandle<T> *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (try_push_inner_(std::move(item))) {
            return true;
        }
        finish_handle->set_item(std::move(item));
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(finish_handle);
        return false;
    }

    bool cancel_push(ChannelPushFinishHandle<T> *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return push_awaiters_.remove(finish_handle);
    }

    std::optional<T> request_pop(ChannelPopFinishHandle<T> *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = try_pop_inner_();
        if (result.has_value()) {
            return result;
        }
        assert(push_awaiters_.empty());
        pop_awaiters_.push_back(finish_handle);
        return std::nullopt;
    }

    bool cancel_pop(ChannelPopFinishHandle<T> *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_awaiters_.remove(finish_handle);
    }

private:
    template <typename U> bool try_push_inner_(U &&item) {
        if (empty_inner_() && !pop_awaiters_.empty()) {
            auto *pop_handle = pop_awaiters_.pop_front();
            pop_handle->set_result(std::forward<U>(item));
            schedule_(pop_handle);
            return true;
        }
        if (!full_inner_()) {
            push_inner_(std::move(item));
            return true;
        }
        return false;
    }

    std::optional<T> try_pop_inner_() {
        if (full_inner_() && !push_awaiters_.empty()) {
            auto *push_handle = push_awaiters_.pop_front();
            T item = std::move(push_handle->get_item());
            schedule_(push_handle);
            T result = pop_inner_();
            push_inner_(std::move(item));
            return result;
        }
        if (!empty_inner_()) {
            T result = pop_inner_();
            return result;
        }
        return std::nullopt;
    }

    template <typename U> void push_inner_(U &&item) {
        assert(!full_inner_());
        buffer_[tail_].emplace(std::forward<U>(item));
        tail_ = (tail_ + 1) % actual_capacity_;
    }

    T pop_inner_() {
        assert(!empty_inner_());
        T item = std::move(buffer_[head_].get());
        buffer_[head_].reset();
        head_ = (head_ + 1) % actual_capacity_;
        return item;
    }

    bool empty_inner_() const noexcept { return head_ == tail_; }

    bool full_inner_() const noexcept {
        return (tail_ + 1) % actual_capacity_ == head_;
    }

    template <typename Handle> static void schedule_(Handle *handle) {
        auto *runtime = handle->get_runtime();
        if (runtime == nullptr) [[unlikely]] {
            // Fake handle, no need to schedule
            delete handle;
        } else {
            runtime->schedule(handle);
        }
    }

private:
    template <typename Handle>
    using DoubleList = IntrusiveDoubleList<Handle, &Handle::link_entry_>;

    mutable std::mutex mutex_;
    DoubleList<ChannelPushFinishHandle<T>> push_awaiters_;
    DoubleList<ChannelPopFinishHandle<T>> pop_awaiters_;
    std::unique_ptr<Uninitialized<T>[]> buffer_;
    const size_t actual_capacity_;
    size_t head_ = 0;
    size_t tail_ = 0;
};

template <typename T> void ChannelPushFinishHandle<T>::cancel() {
    if (channel_->cancel_push(this)) {
        // Successfully canceled
        Context::current().runtime()->resume_work();
        (*invoker_)();
    }
}

template <typename T> void ChannelPopFinishHandle<T>::cancel() {
    if (channel_->cancel_pop(this)) {
        // Successfully canceled
        Context::current().runtime()->resume_work();
        (*invoker_)();
    }
}

template <typename T> struct Channel<T>::ChannelPushAwaiter {
public:
    using HandleType = ChannelPushFinishHandle<T>;

    ChannelPushAwaiter(Channel<T> &channel, T item)
        : channel_(channel), item_(std::move(item)) {}
    ChannelPushAwaiter(ChannelPushAwaiter &&) = default;

    ChannelPushAwaiter(const ChannelPushAwaiter &) = delete;
    ChannelPushAwaiter &operator=(const ChannelPushAwaiter &) = delete;
    ChannelPushAwaiter &operator=(ChannelPushAwaiter &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) {
        auto *runtime = Context::current().runtime();
        runtime->pend_work();
        finish_handle_.set_channel(&channel_);
        finish_handle_.set_runtime(runtime);
        bool ok = channel_.request_push(std::move(item_), &finish_handle_);
        if (ok) {
            runtime->schedule(&finish_handle_);
        }
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> h) {
        Context::current().runtime()->pend_work();
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        finish_handle_.set_channel(&channel_);
        finish_handle_.set_runtime(Context::current().runtime());
        bool ok = channel_.request_push(std::move(item_), &finish_handle_);
        if (ok) {
            Context::current().runtime()->resume_work();
        }
        bool do_suspend = !ok;
        return do_suspend;
    }

    void await_resume() { /* No result to return */ }

private:
    Channel<T> &channel_;
    T item_;
    ChannelPushFinishHandle<T> finish_handle_;
};

template <typename T> struct Channel<T>::ChannelPopAwaiter {
public:
    using HandleType = ChannelPopFinishHandle<T>;

    ChannelPopAwaiter(Channel<T> &channel) : channel_(channel) {}
    ChannelPopAwaiter(ChannelPopAwaiter &&) = default;

    ChannelPopAwaiter(const ChannelPopAwaiter &) = delete;
    ChannelPopAwaiter &operator=(const ChannelPopAwaiter &) = delete;
    ChannelPopAwaiter &operator=(ChannelPopAwaiter &&) = delete;

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) {
        auto *runtime = Context::current().runtime();
        runtime->pend_work();
        finish_handle_.set_channel(&channel_);
        finish_handle_.set_runtime(runtime);
        auto item = channel_.request_pop(&finish_handle_);
        if (item.has_value()) {
            finish_handle_.set_result(std::move(item.value()));
            runtime->schedule(&finish_handle_);
        }
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> h) {
        Context::current().runtime()->pend_work();
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        finish_handle_.set_channel(&channel_);
        finish_handle_.set_runtime(Context::current().runtime());
        auto item = channel_.request_pop(&finish_handle_);
        if (item.has_value()) {
            Context::current().runtime()->resume_work();
            finish_handle_.set_result(std::move(item.value()));
            return false; // Do not suspend
        }
        return true; // Suspend
    }

    T await_resume() { return finish_handle_.extract_result(); }

private:
    Channel<T> &channel_;
    ChannelPopFinishHandle<T> finish_handle_;
};

} // namespace condy