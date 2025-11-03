#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/runtime_options.hpp"
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
    virtual void done() = 0;

    virtual void wait() = 0;

    virtual void schedule(WorkInvoker *work) = 0;

    virtual void pend_work() = 0;

    virtual void resume_work() = 0;

    virtual bool is_single_thread() const = 0;
};

using WorkListQueue =
    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>;

class SingleThreadRuntime : public IRuntime {
public:
    SingleThreadRuntime(const SingleThreadOptions &options) {
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));

        params.flags |= IORING_SETUP_CLAMP;
#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif

        size_t ring_entries = options.sq_size_;
        if (options.cq_size_ != ring_entries * 2) {
            params.flags |= IORING_SETUP_CQSIZE;
            params.cq_entries = options.cq_size_;
        }

        if (options.enable_sqpoll_) {
            params.flags |= IORING_SETUP_SQPOLL;
            params.sq_thread_idle = options.sqpoll_idle_time_ms_;
        }

#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        if (options.enable_defer_taskrun_) {
            params.flags |= IORING_SETUP_DEFER_TASKRUN;
        }
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
        if (options.enable_coop_taskrun_) {
            params.flags |= IORING_SETUP_COOP_TASKRUN;
        }
#endif

        if (options.enable_iopoll_) {
            params.flags |= IORING_SETUP_IOPOLL;
#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
            if (options.iopoll_hybrid_) {
                params.flags |= IORING_SETUP_HYBRID_IOPOLL;
            }
#endif
        }
        ring_.init(ring_entries, &params);
        ring_.set_submit_batch_size(options.submit_batch_size_);

        global_queue_interval_ = options.global_queue_interval_;
        event_interval_ = options.event_interval_;

        idle_time_us_ = options.idle_time_us_;
    }

    ~SingleThreadRuntime() { ring_.destroy(); }

    SingleThreadRuntime(const SingleThreadRuntime &) = delete;
    SingleThreadRuntime &operator=(const SingleThreadRuntime &) = delete;
    SingleThreadRuntime(SingleThreadRuntime &&) = delete;
    SingleThreadRuntime &operator=(SingleThreadRuntime &&) = delete;

public:
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
        auto d = defer([]() { Context::current().reset(); });

        while (true) {
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
                    return !global_queue_.empty() ||
                           (done_ && pending_works_ == 0);
                });
                return true;
            }
        }

        // 5. Now we have no work for now, but there's some outstanding ops in
        // the ring. We wait them with a timeout.
        ring_.reap_completions([this](io_uring_cqe *cqe) { process_cqe_(cqe); },
                               idle_time_us_);

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

    std::mutex mutex_;
    std::condition_variable cv_;
    WorkListQueue global_queue_;

    WorkListQueue local_queue_;

    Ring ring_;

    // When pending_works_ > 0, it means there are some works going to be posted
    // to the runtime, so the runtime should not exit even if done_ is set.
    size_t pending_works_ = 0;

    size_t tick_count_ = 0;

    size_t global_queue_interval_ = 31;
    size_t event_interval_ = 61;
    size_t idle_time_us_ = 1000000;
};

class MultiThreadRuntime : public IRuntime {
public:
    MultiThreadRuntime(const MultiThreadOptions &options)
        : shuffle_gen_(options.num_threads_),
          local_data_(std::make_unique<LocalData[]>(options.num_threads_)) {
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));

        params.flags |= IORING_SETUP_CLAMP;

        size_t ring_entries = options.sq_size_;
        if (options.cq_size_ != ring_entries * 2) {
            params.flags |= IORING_SETUP_CQSIZE;
            params.cq_entries = options.cq_size_;
        }

        if (options.enable_sqpoll_) {
            params.flags |= IORING_SETUP_SQPOLL;
            params.sq_thread_idle = options.sqpoll_idle_time_ms_;
        }

#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        if (options.enable_defer_taskrun_) {
            params.flags |= IORING_SETUP_DEFER_TASKRUN;
        }
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
        if (options.enable_coop_taskrun_) {
            params.flags |= IORING_SETUP_COOP_TASKRUN;
        }
#endif

        if (options.enable_iopoll_) {
            params.flags |= IORING_SETUP_IOPOLL;
#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
            if (options.iopoll_hybrid_) {
                params.flags |= IORING_SETUP_HYBRID_IOPOLL;
            }
#endif
        }

        PCG32 base_pcg32(options.random_seed_);

        auto init_ring = [&](Ring &ring) {
            ring.init(ring_entries, &params);
            ring.set_submit_batch_size(options.submit_batch_size_);
            ring.set_use_mutex(true);
        };

        auto init_data = [&](size_t index, LocalData &data) {
            init_ring(data.ring);
            data.thread_index = index;
            data.pcg32 = PCG32(base_pcg32.next());
        };

        init_data(0, local_data_[0]);

        params.flags |= IORING_SETUP_ATTACH_WQ;
        params.wq_fd = local_data_[0].ring.ring_fd();

        for (size_t i = 1; i < options.num_threads_; i++) {
            init_data(i, local_data_[i]);
        }

        num_threads_ = options.num_threads_;
        global_queue_interval_ = options.global_queue_interval_;
        event_interval_ = options.event_interval_;
        self_steal_interval_ = options.self_steal_interval_;
        idle_time_us_ = options.idle_time_us_;

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

        Context::current().init(&data_->ring, this, schedule_local_);
        auto d = defer([]() { Context::current().reset(); });

        while (true) {
            data_->tick_count++;

            if (data_->tick_count % event_interval_ == 0) {
                flush_ring_();
            }

            auto *work = next_();
            if (work) {
                (*work)();
                continue;
            }

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
    }

    static void schedule_local_(IRuntime *runtime, WorkInvoker *work) {
        auto *self = static_cast<MultiThreadRuntime *>(runtime);
        if (!self->data_->local_queue.try_push(work)) {
            self->data_->extended_queue.push_back(work);
            self->cv_.notify_all();
        } else {
            self->cv_.notify_one();
        }
    }

    size_t flush_ring_() {
        return data_->ring.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle->set_result(cqe->res);
        if (!data_->local_queue.try_push(handle)) {
            data_->extended_queue.push_back(handle);
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
        WorkInvoker *work;
        data_->local_tick_count++;
        if (data_->local_tick_count % self_steal_interval_ == 0) {
            work = data_->local_queue.steal();
        } else {
            work = data_->local_queue.pop();
        }
        if (work) {
            return work;
        }
        return next_extended_flush_();
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

    WorkInvoker *next_extended_flush_() {
        WorkInvoker *work = data_->extended_queue.pop_front();
        if (!work) {
            return nullptr;
        }
        WorkInvoker *next_work;
        while ((next_work = data_->extended_queue.front()) != nullptr &&
               data_->local_queue.try_push(next_work)) {
            auto *tmp = data_->extended_queue.pop_front();
            assert(tmp == next_work);
        }
        return work;
    }

    size_t flush_global_() {
        // NOTICE: mutex_ must be locked before calling this function
        size_t total = 0;
        WorkInvoker *work = nullptr;
        while ((work = global_queue_.front()) != nullptr &&
               data_->local_queue.try_push(work)) {
            auto *tmp = global_queue_.pop_front();
            assert(tmp == work);
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
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); }, idle_time_us_);

        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    WorkListQueue global_queue_;
    std::atomic_size_t pending_works_ = 0;
    size_t blocking_threads_ = 0;

    size_t global_queue_interval_ = 31;
    size_t event_interval_ = 61;
    size_t self_steal_interval_ = 7;
    size_t idle_time_us_ = 1000000;

    ShuffleGenerator shuffle_gen_;

private:
    size_t num_threads_;
    std::vector<std::thread> threads_;

    struct LocalData {
        size_t thread_index;
        Ring ring;
        BoundedTaskQueue<WorkInvoker *, 8> local_queue;
        WorkListQueue extended_queue;
        size_t tick_count = 0;
        size_t local_tick_count = 0;
        PCG32 pcg32;
    };
    std::unique_ptr<LocalData[]> local_data_ = nullptr;

    inline static thread_local LocalData *data_ = nullptr;
};

} // namespace condy