#pragma once

#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/utils.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/eventfd.h>

namespace condy {

class Ring {
public:
    Ring() = default;
    ~Ring() { destroy(); }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;
    Ring(Ring &&) = delete;
    Ring &operator=(Ring &&) = delete;

public:
    void init(unsigned int entries, io_uring_params *params) {
        int r;
        assert(!initialized_);
        r = io_uring_queue_init_params(entries, &ring_, params);
        if (r < 0) {
            throw std::runtime_error("io_uring_queue_init_params failed: " +
                                     std::string(strerror(-r)));
        }
        sqpoll_mode_ = (params->flags & IORING_SETUP_SQPOLL) != 0;
        initialized_ = true;
    }

    void destroy() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
            initialized_ = false;
        }
    }

    void set_use_mutex(bool use_mutex) { sq_mutex_.set_use_mutex(use_mutex); }

    template <typename Func>
    void register_op(Func &&prep_func, OpFinishHandle *handle) {
        std::lock_guard lock(sq_mutex_);
        io_uring_sqe *sqe = get_sqe_();
        std::move(prep_func)(sqe);
        io_uring_sqe_set_data(sqe, handle);
        outstanding_ops_count_++;
        maybe_submit_();
    }

    void cancel_op(OpFinishHandle *handle) {
        std::lock_guard lock(sq_mutex_);
        io_uring_sqe *sqe = get_sqe_();
        io_uring_prep_cancel(sqe, handle, 0);
        io_uring_sqe_set_data(sqe, IGNORE_DATA);
        maybe_submit_();
    }

    void submit() {
        std::lock_guard lock(sq_mutex_);
        unsubmitted_count_ = 0;
        io_uring_submit(&ring_);
    }

    template <typename Func>
    size_t reap_completions(Func &&process_func, size_t timeout_us = 0) {
        size_t reaped = 0;
        io_uring_cqe *cqe;
        int r;
        if (timeout_us > 0) {
            __kernel_timespec ts;
            ts.tv_sec = timeout_us / 1000000;
            ts.tv_nsec = (timeout_us % 1000000) * 1000;
            r = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
            if (r == -ETIME || r == -EINTR) {
                return 0; // Timeout without any completions
            } else if (r < 0) {
                throw std::runtime_error("io_uring_wait_cqe_timeout failed");
            }

            if (reap_one_(cqe, process_func)) {
                reaped++;
            }
        }
        while ((r = io_uring_peek_cqe(&ring_, &cqe)) == 0) {
            if (reap_one_(cqe, process_func)) {
                reaped++;
            }
        }
        return reaped;
    }

    bool has_outstanding_ops() const { return outstanding_ops_count_ > 0; }

    template <typename Func> size_t wait_all_completions(Func &&process_func) {
        size_t total_reaped = 0;
        submit(); // Ensure all outstanding ops are submitted
        while (has_outstanding_ops()) {
            io_uring_cqe *cqe;
            io_uring_wait_cqe(&ring_, &cqe);
            total_reaped += reap_completions(std::forward<Func>(process_func));
        }
        return total_reaped;
    }

    void reserve_space(size_t n) {
        std::lock_guard lock(sq_mutex_);
        size_t space_left;
        do {
            space_left = io_uring_sq_space_left(&ring_);
            if (space_left >= n) {
                return;
            }
            unsubmitted_count_ = 0;
            io_uring_submit(&ring_);
        } while (1);
    }

    void set_submit_batch_size(size_t size) { submit_batch_size_ = size; }

private:
    template <typename Func>
    bool reap_one_(io_uring_cqe *cqe, Func &&process_func) {
        bool unfinished = cqe->flags & IORING_CQE_F_MORE;
        void *data = io_uring_cqe_get_data(cqe);
        if (data == IGNORE_DATA) {
            io_uring_cqe_seen(&ring_, cqe);
            return false;
        }
        if (!unfinished) {
            outstanding_ops_count_--;
        }
        process_func(cqe);
        io_uring_cqe_seen(&ring_, cqe);
        return true;
    }

    io_uring_sqe *get_sqe_() {
        io_uring_sqe *sqe;
        do {
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                break;
            }
            if (!sqpoll_mode_) {
                io_uring_submit(&ring_);
            } else {
                io_uring_sqring_wait(&ring_);
            }
            unsubmitted_count_ = 0;
        } while (1);
        return sqe;
    }

    void maybe_submit_() {
        if (unsubmitted_count_++ >= submit_batch_size_) {
            submit();
        }
    }

public:
    inline static void *const IGNORE_DATA = reinterpret_cast<void *>(0x1);

private:
    // SQ may be accessed from multiple threads, so protect it
    MaybeMutex<std::mutex> sq_mutex_;
    bool initialized_ = false;
    io_uring ring_;
    std::atomic_size_t outstanding_ops_count_ = 0;
    bool sqpoll_mode_ = false;
    size_t unsubmitted_count_ = 0;

    // Configuration
    size_t submit_batch_size_ = 128;
};

} // namespace condy