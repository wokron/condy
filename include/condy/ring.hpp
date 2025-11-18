#pragma once

#include "condy/condy_uring.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace condy {

class FdTable {
public:
    FdTable(io_uring &ring) : ring_(ring) {}

    FdTable(const FdTable &) = delete;
    FdTable &operator=(const FdTable &) = delete;
    FdTable(FdTable &&) = delete;
    FdTable &operator=(FdTable &&) = delete;

public:
    void init(size_t capacity) {
        int r = io_uring_register_files_sparse(&ring_, capacity);
        if (r < 0) {
            throw std::runtime_error("io_uring_register_files_sparse failed: " +
                                     std::string(strerror(-r)));
        }
        capacity_ = capacity;
        alloc_range_offset_ = 0;
        alloc_range_size_ = capacity;
        initialized_ = true;
    }

    void register_fd(int fixed_fd, int fd) {
        check_initialized_();
        int r = io_uring_register_files_update(&ring_, fixed_fd, &fd, 1);
        if (r < 0) {
            throw std::runtime_error("io_uring_register_files_update failed: " +
                                     std::string(strerror(-r)));
        }
    }

    auto async_register_fd(int *fds, unsigned nr_fds, int offset);

    auto async_get_raw_fd(int fixed_fd, unsigned int flags);

    void unregister_fd(int fixed_fd) {
        check_initialized_();
        int invalid_fd = -1;
        int r =
            io_uring_register_files_update(&ring_, fixed_fd, &invalid_fd, 1);
        if (r < 0) {
            throw std::runtime_error("io_uring_register_files_update failed: " +
                                     std::string(strerror(-r)));
        }
    }

    void set_alloc_range(size_t offset, size_t size) {
        check_initialized_();
        alloc_range_offset_ = offset;
        alloc_range_size_ = size;
        int r = io_uring_register_file_alloc_range(&ring_, offset, size);
        if (r < 0) {
            throw std::runtime_error(
                "io_uring_register_file_alloc_range failed: " +
                std::string(strerror(-r)));
        }
    }

    std::pair<size_t, size_t> get_alloc_range() const {
        return {alloc_range_offset_, alloc_range_size_};
    }

    size_t capacity() const { return capacity_; }

private:
    void check_initialized_() {
        if (!initialized_) {
            throw std::runtime_error("FdTable not initialized");
        }
    }

private:
    bool initialized_ = false;
    size_t capacity_ = 0;
    size_t alloc_range_offset_ = 0;
    size_t alloc_range_size_ = 0;
    io_uring &ring_;
};

class BufferTable {
public:
    BufferTable(io_uring &ring) : ring_(ring) {}

    BufferTable(const BufferTable &) = delete;
    BufferTable &operator=(const BufferTable &) = delete;
    BufferTable(BufferTable &&) = delete;
    BufferTable &operator=(BufferTable &&) = delete;

public:
    void init(size_t capacity) {
        int r = io_uring_register_buffers_sparse(&ring_, capacity);
        if (r < 0) {
            throw std::runtime_error(
                "io_uring_register_buffers_sparse failed: " +
                std::string(strerror(-r)));
        }
        capacity_ = capacity;
        initialized_ = true;
    }

    void register_buffer(int index, const iovec &buf) {
        check_initialized_();
        int r = io_uring_register_buffers_update_tag(&ring_, index, &buf,
                                                     nullptr, 1);
        if (r < 0) {
            throw std::runtime_error(
                "io_uring_register_buffers_update failed: " +
                std::string(strerror(-r)));
        }
    }

    void unregister_buffer(int index) {
        check_initialized_();
        iovec vec{nullptr, 0};
        int r = io_uring_register_buffers_update_tag(&ring_, index, &vec,
                                                     nullptr, 1);
        if (r < 0) {
            throw std::runtime_error(
                "io_uring_register_buffers_update failed: " +
                std::string(strerror(-r)));
        }
    }

    size_t capacity() const { return capacity_; }

private:
    void check_initialized_() {
        if (!initialized_) {
            throw std::runtime_error("BufferTable not initialized");
        }
    }

private:
    bool initialized_ = false;
    size_t capacity_ = 0;
    io_uring &ring_;
};

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

    template <typename Func> void register_op(Func &&prep_func, void *handle) {
        io_uring_sqe *sqe = get_sqe_();
        std::move(prep_func)(sqe);
        io_uring_sqe_set_data(sqe, handle);
        if (handle != IGNORE_DATA && handle != NOTIFY_DATA) {
            outstanding_ops_count_++;
        }
        maybe_submit_();
    }

    void cancel_op(void *handle) {
        io_uring_sqe *sqe = get_sqe_();
        io_uring_prep_cancel(sqe, handle, 0);
        io_uring_sqe_set_data(sqe, IGNORE_DATA);
        maybe_submit_();
    }

    void submit() {
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

    io_uring *ring() { return &ring_; }

    FdTable &fd_table() { return fd_table_; }

    BufferTable &buffer_table() { return buffer_table_; }

private:
    template <typename Func>
    bool reap_one_(io_uring_cqe *cqe, Func &&process_func) {
        void *data = io_uring_cqe_get_data(cqe);
        if (data == IGNORE_DATA) {
            io_uring_cqe_seen(&ring_, cqe);
            return false;
        }
#if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
        bool unfinished = cqe->flags & IORING_CQE_F_MORE;
#else
        bool unfinished = false;
#endif
        if (!unfinished && data != NOTIFY_DATA) {
            outstanding_ops_count_--;
        }
        process_func(cqe);
        io_uring_cqe_seen(&ring_, cqe);
        return true;
    }

    io_uring_sqe *get_sqe_() {
        int r;
        io_uring_sqe *sqe;
        do {
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                break;
            }
            r = io_uring_submit(&ring_);
            if (r < 0) {
                throw std::runtime_error("io_uring_submit failed: " +
                                         std::string(strerror(-r)));
            }
            unsubmitted_count_ = 0;
            if (sqpoll_mode_) {
                r = io_uring_sqring_wait(&ring_);
                if (r < 0) {
                    throw std::runtime_error("io_uring_sqring_wait failed: " +
                                             std::string(strerror(-r)));
                }
            }
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
    inline static void *const NOTIFY_DATA = reinterpret_cast<void *>(0x2);

private:
    // SQ may be accessed from multiple threads, so protect it
    bool initialized_ = false;
    io_uring ring_;
    size_t outstanding_ops_count_ = 0;
    bool sqpoll_mode_ = false;
    size_t unsubmitted_count_ = 0;

    FdTable fd_table_{ring_};
    BufferTable buffer_table_{ring_};

    // Configuration
    size_t submit_batch_size_ = 128;
};

} // namespace condy