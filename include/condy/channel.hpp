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

template <typename T, size_t N = 2> class Channel {
public:
    Channel(size_t capacity) : buffer_(std::bit_ceil(capacity)) {}
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
        auto *fake_handle = new PushFinishHandle(std::forward<U>(item));
        assert(pop_awaiters_.empty());
        push_awaiters_.push_back(fake_handle);
    }

    struct [[nodiscard]] PushAwaiter;
    template <typename U> PushAwaiter push(U &&item) {
        return {*this, std::forward<U>(item)};
    }

    struct [[nodiscard]] PopAwaiter;
    PopAwaiter pop() { return {*this}; }

    size_t capacity() const noexcept { return buffer_.capacity(); }

    size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

    bool is_closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

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
        Context::current().runtime()->pend_work();
        return false;
    }

    std::optional<T> request_pop_(PopFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = try_pop_inner_();
        if (result.has_value()) {
            return result;
        }
        assert(push_awaiters_.empty());
        pop_awaiters_.push_back(finish_handle);
        Context::current().runtime()->pend_work();
        return std::nullopt;
    }

    bool cancel_pop_(PopFinishHandle *finish_handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_awaiters_.remove(finish_handle);
    }

private:
    template <typename U> bool try_push_inner_(U &&item) {
        if (closed_) [[unlikely]] {
            panic_on("Push to closed channel");
        }
        if (!pop_awaiters_.empty()) {
            assert(empty_inner_());
            auto *pop_handle = pop_awaiters_.pop_front();
            pop_handle->set_result(std::forward<U>(item));
            pop_handle->schedule();
            return true;
        }
        if (!full_inner_()) {
            push_inner_(std::move(item));
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
            T result = pop_inner_();
            push_inner_(std::move(item));
            return result;
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

    bool empty_inner_() const noexcept { return size_ == 0; }

    bool full_inner_() const noexcept { return size_ == buffer_.capacity(); }

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
        // If there are pending push awaiters, panic
        if (!push_awaiters_.empty()) {
            panic_on("Channel closed with pending push awaiters");
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
    using PushList =
        IntrusiveSingleList<PushFinishHandle, &PushFinishHandle::link_entry_>;
    using PopList =
        IntrusiveDoubleList<PopFinishHandle, &PopFinishHandle::link_entry_>;

    mutable std::mutex mutex_;
    PushList push_awaiters_;
    PopList pop_awaiters_;
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
    using ReturnType = void;

    PushFinishHandle(T item) : item_(std::move(item)) {}

    ReturnType extract_result() { return; }

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
        if (runtime_ == nullptr) {
            // Fake handle, no need to schedule
            delete this;
        } else {
            need_resume_ = true;
            runtime_->schedule(this);
        }
    }

public:
    SingleLinkEntry link_entry_;

private:
    Invoker *invoker_ = nullptr;
    Channel *channel_ = nullptr;
    Runtime *runtime_ = nullptr;
    T item_;
    bool need_resume_ = false;
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
            (*invoker_)();
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
        auto *runtime = Context::current().runtime();
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
        finish_handle_.init(&channel_, Context::current().runtime());
        bool ok = channel_.request_push_(&finish_handle_);
        bool do_suspend = !ok;
        return do_suspend;
    }

    auto await_resume() { return finish_handle_.extract_result(); }

private:
    Channel &channel_;
    PushFinishHandle finish_handle_;
};

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
        auto *runtime = Context::current().runtime();
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
        finish_handle_.init(&channel_, Context::current().runtime());
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