#pragma once

#include "condy/finish_handles.hpp"
#include "condy/intrusive.hpp"
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
#include <mutex>

namespace condy {

class IRuntime {
public:
    virtual void cancel() = 0;

    virtual void done() = 0;

    virtual void wait() = 0;

    virtual void schedule(WorkInvoker *work) = 0;

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
        std::lock_guard<std::mutex> lock(mutex_);
        canceled_ = true;
        done_ = true;
        cv_.notify_all();
    }

    void done() override {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_.notify_all();
    }

    void schedule(WorkInvoker *work) override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool need_notify = global_queue_.empty();
        global_queue_.push_back(work);
        if (need_notify) {
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

            auto *work = next_();
            if (work) {
                (*work)();
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
    static void schedule_local_(IRuntime *runtime, WorkInvoker *work) {
        auto *self = static_cast<SingleThreadRuntime *>(runtime);
        self->local_queue_.push_back(work);
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

    WorkInvoker *next_() {
        WorkInvoker *work = nullptr;
        if (tick_count_ % global_queue_interval_ == 0) {
            work = next_global_();
            if (!work) {
                work = next_local_();
            }
        } else {
            work = next_local_();
            if (!work) {
                work = next_global_flush_();
            }
        }
        return work;
    }

    WorkInvoker *next_local_() {
        if (local_queue_.empty()) {
            return nullptr;
        }
        return local_queue_.pop_front();
    }

    WorkInvoker *next_global_() {
        std::lock_guard<std::mutex> lock(mutex_);
        return global_queue_.pop_front();
    }

    WorkInvoker *next_global_flush_() {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkInvoker *work = global_queue_.pop_front();
        if (work == nullptr) {
            return nullptr;
        }
        flush_global_();
        return work;
    }

    size_t flush_global_() {
        // NOTICE: mutex_ must be locked before calling this function
        size_t total = 0;
        WorkInvoker *work = nullptr;
        while ((work = global_queue_.pop_front()) != nullptr) {
            local_queue_.push_back(work);
            total++;
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
    bool done_ = false;
    std::atomic_bool canceled_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>
        global_queue_;

    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>
        local_queue_;

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
        assert(num_threads_ >= 2);
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
        std::lock_guard<std::mutex> lock(mutex_);
        canceled_ = true;
        done_ = true;
        cv_.notify_all();
    }

    void done() override {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_.notify_all();
    }

    void schedule(WorkInvoker *work) override {
        std::lock_guard<std::mutex> lock(mutex_);
        global_queue_.push_back(work);
        cv_.notify_one();
    }

    void pend_work() override { pending_works_++; }

    void resume_work() override { pending_works_--; }

    void wait() override {
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

            auto *work = next_();
            if (work) {
                (*work)();
                continue;
            }

            // TODO: Some work cannot be stolen, e.g., cancellable ops.
            work = steal_work_();
            if (work) {
                (*work)();
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

    static void schedule_local_(IRuntime *runtime, WorkInvoker *work) {
        auto *self = static_cast<MultiThreadRuntime *>(runtime);
        self->data_->local_queue.push(work);
        self->cv_.notify_one();
    }

    size_t flush_ring_() {
        return data_->ring.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle->set_result(cqe->res);
        if (handle->is_stealable()) {
            data_->local_queue.push(handle);
        } else {
            data_->no_steal_queue.push_back(handle);
        }
    }

    WorkInvoker *next_() {
        WorkInvoker *work = nullptr;
        if (data_->tick_count % global_queue_interval_ == 0) {
            work = next_global_();
            if (!work) {
                work = next_local_();
            }
        } else {
            work = next_local_();
            if (!work) {
                work = next_global_flush_();
            }
        }
        return work;
    }

    WorkInvoker *next_local_() {
        WorkInvoker *work = data_->no_steal_queue.pop_front();
        if (!work) {
            work = data_->local_queue.pop();
        }
        return work;
    }

    WorkInvoker *next_global_() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_queue_.empty()) {
            return nullptr;
        }
        return global_queue_.pop_front();
    }

    WorkInvoker *next_global_flush_() {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkInvoker *work = global_queue_.pop_front();
        if (!work) {
            return nullptr;
        }
        flush_global_();
        return work;
    }

    size_t flush_global_() {
        // NOTICE: mutex_ must be locked before calling this function
        size_t total = 0;
        WorkInvoker *work = nullptr;
        while ((work = global_queue_.pop_front()) != nullptr) {
            data_->local_queue.push(work);
            total++;
        }
        return total;
    }

    WorkInvoker *steal_work_() {
        WorkInvoker *work = nullptr;
        auto r32 = data_->pcg32.next();
        shuffle_gen_.generate(r32, 0, num_threads_, [&, this](uint32_t victim) {
            if (victim == data_->thread_index) {
                return true;
            }
            auto &victim_data = local_data_[victim];
            work = victim_data.local_queue.steal();
            if (work) {
                return false; // Stop iteration
            }
            return true; // Continue iteration
        });
        return work;
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
                if (done_ && pending_works_ == 0 &&
                    blocking_threads_ == num_threads_) {
                    // 3. If done_ is set and there is no more works, we can
                    // exit.
                    cv_.notify_all();
                    return false;
                }
                // 4. No outstanding ops in the ring, we can block here safely.
                cv_.wait(lock);
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
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic_bool canceled_ = false;
    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>
        global_queue_;
    std::atomic_size_t pending_works_ = 0;
    size_t blocking_threads_ = 0;

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
        UnboundedTaskQueue<WorkInvoker *> local_queue{8};
        IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>
            no_steal_queue;
        size_t tick_count = 0;
        PCG32 pcg32;
    };
    std::unique_ptr<LocalData[]> local_data_;

    inline static thread_local LocalData *data_;
};

} // namespace condy