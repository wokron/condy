#pragma once

#include "condy/condy_uring.hpp"
#include "condy/context.hpp"
#include "condy/finish_handles.hpp"
#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/ring.hpp"
#include "condy/runtime_options.hpp"
#include "condy/utils.hpp"
#include "condy/work_type.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/eventfd.h>

namespace condy {

using WorkListQueue =
    IntrusiveSingleList<WorkInvoker, &WorkInvoker::work_queue_entry_>;

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
class AsyncWaiter {
public:
    void async_wait(Ring &) {}

    void notify(Ring &ring) {
        io_uring_sqe sqe = {};
        io_uring_prep_msg_ring(
            &sqe, ring.ring()->ring_fd, 0,
            reinterpret_cast<uint64_t>(encode_work(nullptr, WorkType::Notify)),
            0);
        io_uring_register_sync_msg(&sqe);
    }
};
#else
class AsyncWaiter {
public:
    AsyncWaiter() {
        notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (notify_fd_ < 0) {
            throw make_system_error("eventfd", errno);
        }
    }

    ~AsyncWaiter() { close(notify_fd_); }

    void async_wait(Ring &ring) {
        eventfd_read(notify_fd_, &dummy_);
        io_uring_sqe *sqe = ring.get_sqe();
        io_uring_prep_read(sqe, notify_fd_, &dummy_, sizeof(dummy_), 0);
        io_uring_sqe_set_data(sqe, encode_work(nullptr, WorkType::Notify));
    }

    void notify(Ring &) { eventfd_write(notify_fd_, 1); }

private:
    int notify_fd_;
    eventfd_t dummy_;
};
#endif

class Runtime {
public:
    Runtime(const RuntimeOptions &options = {}) {
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));

        params.flags |= IORING_SETUP_CLAMP;
        params.flags |= IORING_SETUP_SINGLE_ISSUER;
        params.flags |= IORING_SETUP_SUBMIT_ALL;
        params.flags |= IORING_SETUP_R_DISABLED;

        size_t ring_entries = options.sq_size_;
        if (options.cq_size_ != 0) { // 0 means default
            params.flags |= IORING_SETUP_CQSIZE;
            params.cq_entries = options.cq_size_;
        }

        if (options.enable_iopoll_) {
            params.flags |= IORING_SETUP_IOPOLL;
#if !IO_URING_CHECK_VERSION(2, 9) // >= 2.9
            if (options.enable_hybrid_iopoll_) {
                params.flags |= IORING_SETUP_HYBRID_IOPOLL;
            }
#endif
        }

        if (options.enable_sqpoll_) {
            params.flags |= IORING_SETUP_SQPOLL;
            params.sq_thread_idle = options.sqpoll_idle_time_ms_;
            if (options.sqpoll_thread_cpu_.has_value()) {
                params.flags |= IORING_SETUP_SQ_AFF;
                params.sq_thread_cpu = *options.sqpoll_thread_cpu_;
            }
        }

        if (options.attach_wq_target_ != nullptr) {
            params.flags |= IORING_SETUP_ATTACH_WQ;
            params.wq_fd = options.attach_wq_target_->ring_.ring()->ring_fd;
        }

        if (options.enable_defer_taskrun_) {
            params.flags |= IORING_SETUP_DEFER_TASKRUN;
        }

        if (options.enable_coop_taskrun_) {
            params.flags |= IORING_SETUP_COOP_TASKRUN;
            if (options.enable_coop_taskrun_flag_) {
                params.flags |= IORING_SETUP_TASKRUN_FLAG;
            }
        }

        if (options.enable_sqe128_) {
            params.flags |= IORING_SETUP_SQE128;
        }

        if (options.enable_cqe32_) {
            params.flags |= IORING_SETUP_CQE32;
        }

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
        if (options.enable_sqe_mixed_) {
            params.flags |= IORING_SETUP_SQE_MIXED;
        }
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
        if (options.enable_cqe_mixed_) {
            params.flags |= IORING_SETUP_CQE_MIXED;
        }
#endif

        void *buf = nullptr;
        size_t buf_size = 0;
#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
        if (options.enable_no_mmap_) {
            params.flags |= IORING_SETUP_NO_MMAP;
            buf = options.no_mmap_buf_;
            buf_size = options.no_mmap_buf_size_;
        }
#endif

        int r = ring_.init(ring_entries, &params, buf, buf_size);
        if (r < 0) {
            throw make_system_error("io_uring_queue_init_params", -r);
        }

        event_interval_ = options.event_interval_;
        disable_register_ring_fd_ = options.disable_register_ring_fd_;
    }

    ~Runtime() { ring_.destroy(); }

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

public:
    void allow_exit() {
        pending_works_--;
        notify();
    }

    void notify() { async_waiter_.notify(ring_); }

    void schedule(WorkInvoker *work) {
        auto *runtime = Context::current().runtime();
        if (runtime == this) {
            local_queue_.push_back(work);
            return;
        }

        auto state = state_.load();
        if (runtime != nullptr && state == State::Enabled) {
            __tsan_release(work);
            io_uring_sqe *sqe = runtime->ring_.get_sqe();
            prep_msg_ring_(sqe, work);
            runtime->pend_work();
            return;
        }

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
        if (runtime == nullptr && state == State::Enabled) {
            __tsan_release(work);
            io_uring_sqe sqe = {};
            prep_msg_ring_(&sqe, work);
            [[maybe_unused]] int r = io_uring_register_sync_msg(&sqe);
            assert(r == 0);
            return;
        }
#endif

        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool need_notify = global_queue_.empty();
            global_queue_.push_back(work);
            if (need_notify) {
                notify();
            }
        }
    }

    void pend_work() { pending_works_++; }

    void resume_work() { pending_works_--; }

    void run() {
        State expected = State::Idle;
        if (!state_.compare_exchange_strong(expected, State::Running)) {
            throw std::logic_error("Runtime is already running or stopped");
        }
        auto d1 = defer([this]() { state_.store(State::Stopped); });

        [[maybe_unused]] int r;
        r = io_uring_enable_rings(ring_.ring());
        assert(r == 0);

        state_.store(State::Enabled);

        if (!disable_register_ring_fd_) {
            r = io_uring_register_ring_fd(ring_.ring());
            if (r != 1) { // 1 indicates success for this call
                throw make_system_error("io_uring_register_ring_fd", -r);
            }
        }

        Context::current().init(&ring_, this);
        auto d2 = defer([]() { Context::current().reset(); });

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

            if (pending_works_ == 0) {
                break;
            }
            flush_ring_(true);
        }
    }

    auto &fd_table() { return ring_.fd_table(); }

    auto &buffer_table() { return ring_.buffer_table(); }

    auto features() { return ring_.features(); }

    auto &settings() { return ring_.settings(); }

private:
    void flush_global_queue_() {
        local_queue_.push_back(std::move(global_queue_));
        async_waiter_.async_wait(ring_);
    }

    void prep_msg_ring_(io_uring_sqe *sqe, WorkInvoker *work) {
        auto data = encode_work(work, WorkType::Schedule);
        io_uring_prep_msg_ring(sqe, this->ring_.ring()->ring_fd, 0,
                               reinterpret_cast<uint64_t>(data), 0);
        io_uring_sqe_set_data(sqe, encode_work(nullptr, WorkType::Schedule));
    }

    size_t flush_ring_(bool submit_and_wait = false) {
        return ring_.reap_completions(
            [this](io_uring_cqe *cqe) { process_cqe_(cqe); }, submit_and_wait);
    }

    void process_cqe_(io_uring_cqe *cqe) {
        auto *data_raw = io_uring_cqe_get_data(cqe);
        auto [data, type] = decode_work(data_raw);

        if (type == WorkType::Ignore) {
            // No-op
            assert(cqe->res != -EINVAL); // If EINVAL, something is wrong
        } else if (type == WorkType::Notify) {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_global_queue_();
        } else if (type == WorkType::SendFd) {
            auto &fd_table = ring_.fd_table();
            if (fd_table.fd_accepter_ == nullptr) [[unlikely]] {
                throw std::logic_error("No way to accept sent fd");
            }
            uint64_t payload = reinterpret_cast<uint64_t>(data) >> 3;
            if (payload == 0) { // Auto-allocate
                fd_table.fd_accepter_(cqe->res);
            } else {
                int target_fd = static_cast<int>(payload - 1);
                fd_table.fd_accepter_(target_fd);
            }
        } else if (type == WorkType::Schedule) {
            if (data == nullptr) {
                assert(cqe->res == 0);
                pending_works_--;
            } else {
                auto *work = static_cast<WorkInvoker *>(data);
                __tsan_acquire(data);
                local_queue_.push_back(work);
            }
        } else if (type == WorkType::MultiShot) {
            auto *handle = static_cast<ExtendOpFinishHandle *>(data);
            handle->set_result(cqe->res, cqe->flags);
            if (cqe->flags & IORING_CQE_F_MORE) {
                handle->invoke_extend(0); // res not used here
            } else {
                pending_works_--;
                local_queue_.push_back(handle);
            }
        } else if (type == WorkType::ZeroCopy) {
            auto *handle = static_cast<ExtendOpFinishHandle *>(data);
            if (cqe->flags & IORING_CQE_F_MORE) {
                handle->set_result(cqe->res, cqe->flags);
                local_queue_.push_back(handle);
            } else {
                pending_works_--;
                if (cqe->flags & IORING_CQE_F_NOTIF) {
                    handle->invoke_extend(cqe->res);
                } else {
                    handle->set_result(cqe->res, cqe->flags);
                    local_queue_.push_back(handle);
                    handle->invoke_extend(0);
                }
            }
        } else if (type == WorkType::Common) {
            auto *handle = static_cast<OpFinishHandle *>(data);
            handle->set_result(cqe->res, cqe->flags);
            pending_works_--;
            local_queue_.push_back(handle);
        } else {
            assert(false && "Invalid work type");
        }
    }

private:
    enum class State : uint8_t {
        Idle,    // Not running
        Running, // Started running
        Enabled, // Running and ring enabled
        Stopped, // Stopped
    };
    static_assert(std::atomic<State>::is_always_lock_free);

    // Global state
    std::mutex mutex_;
    AsyncWaiter async_waiter_;
    WorkListQueue global_queue_;
    std::atomic_size_t pending_works_ = 1;
    std::atomic<State> state_ = State::Idle;

    // Local state
    WorkListQueue local_queue_;
    Ring ring_;
    size_t tick_count_ = 0;

    // Configurable parameters
    size_t event_interval_ = 61;
    bool disable_register_ring_fd_ = false;
};

inline auto &current_runtime() { return *Context::current().runtime(); }

inline void set_current_cred_id(uint16_t id) {
    Context::current().set_cred_id(id);
}

} // namespace condy