#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/runtime_options.hpp"
#include "condy/utils.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/eventfd.h>

namespace condy {

using WorkListQueue =
    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>;

class Runtime {
public:
    Runtime(const RuntimeOptions &options = {}) {
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

        event_interval_ = options.event_interval_;

        notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (notify_fd_ < 0) {
            throw std::runtime_error("Failed to create eventfd for runtime: " +
                                     std::string(std::strerror(errno)));
        }
    }

    ~Runtime() {
        close(notify_fd_);
        ring_.destroy();
    }

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

public:
    void done() {
        pending_works_--;
        eventfd_write(notify_fd_, 1);
    }

    void schedule(WorkInvoker *work) {
        auto *runtime = Context::current().runtime();
        if (runtime == this) {
            local_queue_.push_back(work);
            return;
        }

        if (runtime != nullptr) {
            runtime->ring_.register_op(
                [work, this](io_uring_sqe *sqe) {
                    __tsan_release(work);
                    io_uring_prep_msg_ring_cqe_flags(
                        sqe, this->ring_.ring()->ring_fd, 0,
                        reinterpret_cast<uint64_t>(work), 0, IORING_CQE_F_MORE);
                    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
                },
                Ring::IGNORE_DATA);
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        global_queue_.push_back(work);
        eventfd_write(notify_fd_, 1);
    }

    void pend_work() { pending_works_++; }

    void resume_work() { pending_works_--; }

    bool is_running() const { return running_.load(); }

    void block_until_running() { running_.wait(false); }

    void wait() {
#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
        io_uring_enable_rings(ring_.ring());
#endif
        running_.store(true);
        running_.notify_all();

        Context::current().init(&ring_, this);
        auto d = defer([]() { Context::current().reset(); });

        {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_global_queue_();
        }

        while (true) {
            tick_count_++;

            if (tick_count_ % event_interval_ == 0) {
                flush_ring_();
            }

            if (auto *work = local_queue_.pop_front()) {
                (*work)();
                continue;
            }

            if (!wait_for_work_()) {
                break;
            }
        }
    }

    size_t next_bgid() { return next_bgid_++; }

private:
    void flush_global_queue_() {
        local_queue_.push_back(std::move(global_queue_));
        eventfd_read(notify_fd_, &dummy_);
        ring_.register_op(
            [this](io_uring_sqe *sqe) {
                io_uring_prep_read(sqe, notify_fd_, &dummy_, sizeof(dummy_), 0);
            },
            Ring::NOTIFY_DATA);
    }

    bool wait_for_work_() {
        ring_.submit();
        auto reaped = flush_ring_();
        if (reaped > 0) {
            // 1. If we got some completions, return immediately.
            return true;
        }

        if (!ring_.has_outstanding_ops() && pending_works_ == 0) {
            // 3. No outstanding ops in the ring, and no more works, we can
            // exit.
            return false;
        }

        // 5. Block until we get some completions (or notification)
        ring_.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });

        return true;
    }

    size_t flush_ring_() {
        return ring_.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); });
    }

    void process_cqe_(io_uring_cqe *cqe) {
        auto *data = io_uring_cqe_get_data(cqe);
        __tsan_acquire(data);
        if (data == Ring::NOTIFY_DATA) {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_global_queue_();
            return;
        }

        auto *work = static_cast<WorkInvoker *>(data);
        if (!work->is_operation()) {
            local_queue_.push_back(work);
            return;
        }

        auto *handle = static_cast<OpFinishHandle *>(work);
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
    std::atomic_bool running_ = false;
    // Global state
    std::mutex mutex_;
    eventfd_t dummy_;
    int notify_fd_;
    WorkListQueue global_queue_;
    std::atomic_size_t pending_works_ = 1;

    // Local state
    WorkListQueue local_queue_;
    Ring ring_;
    size_t tick_count_ = 0;
    uint16_t next_bgid_ = 0;

    // Configurable parameters
    size_t event_interval_ = 61;
};

} // namespace condy