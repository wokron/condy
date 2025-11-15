#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/runtime_options.hpp"
#include "condy/utils.hpp"
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
    SingleThreadRuntime(const SingleThreadOptions &options = {}) {
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));

        params.flags |= IORING_SETUP_CLAMP;
#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        params.flags |= IORING_SETUP_SINGLE_ISSUER;
        params.flags |= IORING_SETUP_R_DISABLED;
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
        pending_works_--;
        cv_.notify_one();
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

#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        io_uring_enable_rings(ring_.ring());
#endif

        Context::current().init(&ring_, this, schedule_local_, next_bgid_func_);
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

    static size_t next_bgid_func_(IRuntime *runtime) {
        auto *self = static_cast<SingleThreadRuntime *>(runtime);
        return self->next_bgid_++;
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
            bool flushed = !global_queue_.empty();
            local_queue_.push_back(std::move(global_queue_));
            if (flushed) {
                // 2. If we got some new work from the global queue, return
                // immediately.
                return true;
            }

            if (!ring_.has_outstanding_ops()) {
                if (pending_works_ == 0) {
                    // 3. If there is no more works, we can
                    // exit.
                    return false;
                }
                // 4. No outstanding ops in the ring, we can block here safely.
                cv_.wait(lock, [this]() {
                    return !global_queue_.empty() || pending_works_ == 0;
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
        local_queue_.push_back(std::move(global_queue_));
        return work;
    }

    size_t flush_ring_() {
        return ring_.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        OpFinishHandle *handle =
            static_cast<OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        handle->set_result(cqe->res, cqe->flags);
#if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
        if (cqe->flags & IORING_CQE_F_MORE) {
            handle->multishot();
            return;
        }
#endif
        if (cqe->flags & IORING_CQE_F_NOTIF) {
            // Notify cqe, no need to schedule back to local queue
            (*handle)();
            return;
        }
        local_queue_.push_back(handle);
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    WorkListQueue global_queue_;

    WorkListQueue local_queue_;

    Ring ring_;

    // Initialized to 1 to prevent premature exit
    size_t pending_works_ = 1;

    size_t tick_count_ = 0;

    uint16_t next_bgid_ = 0;

    // Configuration parameters
    size_t global_queue_interval_ = 31;
    size_t event_interval_ = 61;
    size_t idle_time_us_ = 1000;
};

} // namespace condy