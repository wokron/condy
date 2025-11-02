#pragma once

#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/shuffle_generator.hpp"
#include "condy/utils.hpp"
#include "condy/wsqueue.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/poll.h>

namespace condy {

class IRuntime {
public:
    virtual void cancel() = 0;

    virtual void done() = 0;

    virtual void wait() = 0;

    virtual void schedule(Invoker *invoker) = 0;

    virtual void pend_work() = 0;

    virtual void resume_work() = 0;

    virtual bool is_single_thread() const = 0;
};

class SingleThreadRuntime : public IRuntime {
public:
    SingleThreadRuntime(unsigned int ring_entries) {
        // TODO: Make params configurable
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        ring_.init(ring_entries, &params);
    }

    ~SingleThreadRuntime() { ring_.destroy(); }

    SingleThreadRuntime(const SingleThreadRuntime &) = delete;
    SingleThreadRuntime &operator=(const SingleThreadRuntime &) = delete;
    SingleThreadRuntime(SingleThreadRuntime &&) = delete;
    SingleThreadRuntime &operator=(SingleThreadRuntime &&) = delete;

public:
    void cancel() override {
        canceled_ = true;
        done_ = true;
        cv_.notify_all();
    }

    void done() override {
        done_ = true;
        cv_.notify_all();
    }

    void schedule(Invoker *invoker) override {
        std::lock_guard<std::mutex> lock(mutex_);
        global_queue_.push_back(invoker);
        if (global_queue_.size() == 1) {
            cv_.notify_one();
        }
    }

    void pend_work() override { pending_works_++; }

    void resume_work() override { pending_works_--; }

    void wait() override {
        // In SingleThreadRuntime, wait is the place to run the event loop.

        Context::current().init(&ring_, this, schedule_local_);
        auto d = defer([]() { Context::current().destroy(); });

        while (!canceled_) {
            tick_count_++;

            if (tick_count_ % event_interval_ == 0) {
                flush_ring_();
            }

            auto *invoker = next_();
            if (invoker) {
                (*invoker)();
                continue;
            }

            bool ok = wait_for_work_();
            if (!ok) {
                break;
            }
        }

        if (canceled_) {
            ring_.cancel_all_ops();
            ring_.wait_all_completions([](auto) { /* No-op */ });
        }
    }

    bool is_single_thread() const override { return true; }

private:
    static void schedule_local_(IRuntime *runtime, Invoker *invoker) {
        auto *self = static_cast<SingleThreadRuntime *>(runtime);
        self->local_queue_.push_back(invoker);
    }

    bool wait_for_work_() {
        ring_.submit();
        auto reaped = flush_ring_();
        if (reaped > 0) {
            // 1. If we got some completions, return immediately.
            return true;
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto flushed = flush_global_();
            if (flushed > 0) {
                // 2. If we got some new work from the global queue, return
                // immediately.
                return true;
            }

            if (!ring_.has_outstanding_ops()) {
                if (done_ && pending_works_ == 0) {
                    // 3. If done_ is set and there is no more works, we can
                    // exit.
                    return false;
                }
                // 4. No outstanding ops in the ring, we can block here safely.
                cv_.wait(lock, [this]() {
                    return !global_queue_.empty() || canceled_ ||
                           (done_ && pending_works_ == 0);
                });
                return true;
            }
        }

        // 5. Now we have no work for now, but there's some outstanding ops in
        // the ring. We wait them with a timeout.
        ring_.reap_completions([this](io_uring_cqe *cqe) { process_cqe_(cqe); },
                               1000); // 1ms // TODO: Make timeout configurable

        return true;
    }

    Invoker *next_() {
        Invoker *invoker = nullptr;
        if (tick_count_ % global_queue_interval_ == 0) {
            invoker = next_global_();
            if (!invoker) {
                invoker = next_local_();
            }
        } else {
            invoker = next_local_();
            if (!invoker) {
                invoker = next_global_flush_();
            }
        }
        return invoker;
    }

    Invoker *next_local_() {
        if (local_queue_.empty()) {
            return nullptr;
        }
        Invoker *invoker = local_queue_.front();
        local_queue_.pop_front();
        return invoker;
    }

    Invoker *next_global_() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_queue_.empty()) {
            return nullptr;
        }
        Invoker *invoker = global_queue_.front();
        global_queue_.pop_front();
        return invoker;
    }

    Invoker *next_global_flush_() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_queue_.empty()) {
            return nullptr;
        }
        Invoker *invoker = global_queue_.front();
        global_queue_.pop_front();
        flush_global_();
        return invoker;
    }

    size_t flush_global_() {
        // NOTICE: mutex_ must be locked before calling this function
        size_t total = global_queue_.size();
        while (!global_queue_.empty()) {
            local_queue_.push_back(global_queue_.front());
            global_queue_.pop_front();
        }
        return total;
    }

    size_t flush_ring_() {
        return ring_.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle->set_result(cqe->res);
        local_queue_.push_back(handle);
    }

private:
    std::atomic_bool done_ = false;
    std::atomic_bool canceled_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Invoker *> global_queue_;

    std::deque<Invoker *> local_queue_;

    Ring ring_;

    // When pending_works_ > 0, it means there are some works going to be posted
    // to the runtime, so the runtime should not exit even if done_ is set.
    size_t pending_works_ = 0;

    const size_t global_queue_interval_ = 31;
    const size_t event_interval_ = 61;
    size_t tick_count_ = 0;
};

class MultiThreadRuntime : public IRuntime {
public:
    MultiThreadRuntime(unsigned int ring_entries_per_thread, size_t num_threads)
        : ring_entries_per_thread_(ring_entries_per_thread),
          num_threads_(num_threads),
          local_data_(std::make_unique<LocalData[]>(num_threads)),
          shuffle_gen_(num_threads) {

        threads_.reserve(num_threads_);
        for (size_t i = 0; i < num_threads_; i++) {
            threads_.emplace_back([this, i]() { worker_loop_(i); });
        }
    }

    ~MultiThreadRuntime() {
        done();
        wait();
        for (size_t i = 0; i < num_threads_; i++) {
            local_data_[i].ring.destroy();
        }
    }

    MultiThreadRuntime(const MultiThreadRuntime &) = delete;
    MultiThreadRuntime &operator=(const MultiThreadRuntime &) = delete;
    MultiThreadRuntime(MultiThreadRuntime &&) = delete;
    MultiThreadRuntime &operator=(MultiThreadRuntime &&) = delete;

public:
    void cancel() override {
        canceled_ = true;
        done_ = true;
        cv_.notify_all();
    }

    void done() override {
        done_ = true;
        cv_.notify_all();
    }

    void schedule(Invoker *invoker) override {
        std::lock_guard<std::mutex> lock(mutex_);
        global_queue_.push_back(invoker);
        cv_.notify_one();
    }

    void pend_work() override { data_->pending_works++; }

    void resume_work() override { data_->pending_works--; }

    void wait() override {
        assert(done_ || canceled_);
        for (auto &thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    bool is_single_thread() const override { return false; }

private:
    void worker_loop_(size_t index) {
        data_ = &local_data_[index];
        data_->thread_index = index;
        data_->pcg32 = PCG32(static_cast<uint64_t>(
            std::hash<std::thread::id>()(std::this_thread::get_id())));

        // TODO: Make params configurable
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        data_->ring.init(ring_entries_per_thread_, &params);

        Context::current().init(&data_->ring, this, schedule_local_);
        auto d = defer([]() { Context::current().destroy(); });

        while (!canceled_) {
            data_->tick_count++;

            if (data_->tick_count % event_interval_ == 0) {
                flush_ring_();
            }

            auto *invoker = next_();
            if (invoker) {
                (*invoker)();
                continue;
            }

            // TODO: Some work cannot be stolen, e.g., cancellable ops.
            invoker = steal_work_();
            if (invoker) {
                (*invoker)();
                continue;
            }

            bool ok = wait_for_work_();
            if (!ok) {
                break;
            }
        }

        if (canceled_) {
            data_->ring.cancel_all_ops();
            data_->ring.wait_all_completions([](auto) { /* No-op */ });
        }
    }

    static void schedule_local_(IRuntime *runtime, Invoker *invoker) {
        auto *self = static_cast<MultiThreadRuntime *>(runtime);
        self->data_->local_queue.push(invoker);
    }

    size_t flush_ring_() {
        return data_->ring.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle->set_result(cqe->res);
        data_->local_queue.push(handle);
    }

    Invoker *next_() {
        Invoker *invoker = nullptr;
        if (data_->tick_count % global_queue_interval_ == 0) {
            invoker = next_global_();
            if (!invoker) {
                invoker = next_local_();
            }
        } else {
            invoker = next_local_();
            if (!invoker) {
                invoker = next_global_flush_();
            }
        }
        return invoker;
    }

    Invoker *next_local_() {
        if (data_->local_queue.empty()) {
            return nullptr;
        }
        return data_->local_queue.pop();
    }

    Invoker *next_global_() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_queue_.empty()) {
            return nullptr;
        }
        Invoker *invoker = global_queue_.front();
        global_queue_.pop_front();
        return invoker;
    }

    Invoker *next_global_flush_() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_queue_.empty()) {
            return nullptr;
        }
        Invoker *invoker = global_queue_.front();
        global_queue_.pop_front();
        flush_global_();
        return invoker;
    }

    size_t flush_global_() {
        // NOTICE: mutex_ must be locked before calling this function
        size_t total = global_queue_.size();
        while (!global_queue_.empty()) {
            data_->local_queue.push(global_queue_.front());
            global_queue_.pop_front();
        }
        return total;
    }

    Invoker *steal_work_() {
        Invoker *invoker = nullptr;
        auto r32 = data_->pcg32.next();
        shuffle_gen_.generate(r32, 0, num_threads_, [&, this](uint32_t victim) {
            if (victim == data_->thread_index) {
                return true;
            }
            auto &victim_data = local_data_[victim];
            invoker = victim_data.local_queue.steal();
            if (invoker) {
                return false; // Stop iteration
            }
            return true; // Continue iteration
        });
        return invoker;
    }

    bool wait_for_work_() {
        data_->ring.submit();
        auto reaped = flush_ring_();
        if (reaped > 0) {
            // 1. If we got some completions, return immediately.
            return true;
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto flushed = flush_global_();
            if (flushed > 0) {
                // 2. If we got some new work from the global queue, return
                // immediately.
                return true;
            }

            if (!data_->ring.has_outstanding_ops()) {
                blocking_threads_++;
                if (done_ && data_->pending_works == 0 &&
                    blocking_threads_ == num_threads_) {
                    // 3. If done_ is set and there is no more works, we can
                    // exit.
                    cv_.notify_all();
                    return false;
                }
                // 4. No outstanding ops in the ring, we can block here safely.
                cv_.wait(lock, [this]() {
                    return !global_queue_.empty() || canceled_ ||
                           (done_ && data_->pending_works == 0 &&
                            blocking_threads_ == num_threads_);
                });
                blocking_threads_--;
                return true;
            }
        }

        // 5. Now we have no work for now, but there's some outstanding ops in
        // the ring. We wait them with a timeout.
        data_->ring.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); },
            1000); // 1ms // TODO: Make timeout configurable

        return true;
    }

private:
    std::atomic_bool done_ = false;
    std::atomic_bool canceled_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Invoker *> global_queue_;

    std::atomic_size_t blocking_threads_ = 0;

    const size_t global_queue_interval_ = 31;
    const size_t event_interval_ = 61;
    unsigned int ring_entries_per_thread_;

    ShuffleGenerator shuffle_gen_;

private:
    size_t num_threads_;
    std::vector<std::thread> threads_;

    struct LocalData {
        size_t thread_index;
        Ring ring;
        UnboundedTaskQueue<Invoker *> local_queue{8};
        size_t pending_works = 0;
        size_t tick_count = 0;
        PCG32 pcg32;
    };
    std::unique_ptr<LocalData[]> local_data_;

    inline static thread_local LocalData *data_;
};

} // namespace condy